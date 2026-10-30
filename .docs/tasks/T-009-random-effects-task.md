# T-009: Run random spans on the controller

<!-- File: .docs/tasks/T-009-random-effects-task.md. Branch: feature/T-009-random-effects-task. PR title: "T-009: Run random spans on the controller". -->

## Context

This is the random span events project. See `PLAN.md` § Random span events, the AstrOs.Server spec `.docs/specs/20261022-0748-random-span-events-design.md` § "Firmware", and `.docs/protocol.md` § C.

This task covers the queue producer/consumer seam and the hardware integration. It consumes:

- the T-007 parsers (`RandomCommands.hpp`)
- the T-008 math and table (`RandomEffects.hpp`, `PercentFromPosition`)

It does three things:

- the dispatch loop hands span commands to a new random-effects task
- that task turns due changes into ordinary type-1 / type-5 command strings on the existing servo and GPIO queues
- panic and config reload stop every span

**Depends on T-007 and T-008 merged.** Bench sign-off also needs AstrOs.Server T-007 and T-008 on the bench server, so that a deployed script can carry a span.

## Contract (pinned — do not change)

- **`.docs/protocol.md` § C behavior:**
  - The first change happens at the start, and there is no change at or after `start + spanMs`.
  - A new span on the same channel replaces the old one.
  - Panic and reload stop all spans.
  - `RUN_COMMAND` and `SERVO_TEST` do **not** stop a span.
  - Invalid commands are logged and skipped, and their delta is kept.
- **Queue ownership** (CLAUDE.md): the producer mallocs and frees on a failed send, and the consumer frees after processing.
- **Panic order** (T-004) is preserved. Random spans are cleared **before** step 2 drains `servoQueue`, so no random move can be enqueued after the drain.
- **Existing command types and paths are unchanged.** Changes reach hardware only through `servoQueue` / `gpioQueue`, using the existing type-1 / type-5 string formats.

## Task

### `src/main.cpp`

**Queue and message:**

- Add `randomQueue`, with depth 10 and items of `queue_random_msg_t { MODULE_TYPE type; uint32_t dispatchTickMs; char *data; }`. The dispatch loop already knows the type, so the random task never re-derives it from the string.
- `dispatchTickMs` is `esp_timer_get_time() / 1000` taken in the dispatch loop.

**Dispatch switch:**

- Add `case MODULE_TYPE::MAESTRO_RANDOM: case MODULE_TYPE::GPIO_RANDOM:`.
- Copy `val` (malloc + NUL), stamp `type` and `dispatchTickMs`, and `xQueueSend(randomQueue, …, pdMS_TO_TICKS(100))`.
- The switch's `default:` now logs `ESP_LOGW(TAG, "Unknown command type %d skipped (delta kept)", ...)`. Once 6 and 7 have cases, only truly unknown types reach it. The `NONE` buffer commands keep their own case and stay silent.
- On failure, free the copy and `ESP_LOGW`. The dispatch loop never waits longer than 100 ms on a span.

**`randomEffectsTask`** (stack 4096, priority 8, core 1) owns `RandomSpanTable table(RANDOM_TABLE_CAPACITY, esp_random)`, with `constexpr size_t RANDOM_TABLE_CAPACITY = 32`. It also owns a `std::vector<SpanChange> changes`, reserved to `RANDOM_TABLE_CAPACITY` once at task start; `Collect` emits at most one change per slot. The vector is `clear()`ed each pass, so the loop never allocates. It loops:

1. **Wait.** It does `xQueueReceive(randomQueue, …, waitTicks)`, where `waitTicks` comes from the end of the previous pass (250 ms on the first). **The wait rounds up to whole ticks:** `(ms + portTICK_PERIOD_MS − 1) / portTICK_PERIOD_MS`.
   - Why: at `CONFIG_FREERTOS_HZ=100`, `pdMS_TO_TICKS` truncates a 1–9 ms wait to 0 ticks. This priority-8 task on core 1 would then spin until the change was due, starving `animation_dispatch_task` and IDLE1, which the task watchdog checks. (Amended 2026-10-22, from the T-008 review.)
2. **On a message: the checks outside the table lock.**
   - Parse it with T-007, choosing the parser by `msg.type`.
   - For type 6:
     - Take **one** snapshot of the Maestro channel (below). Use it both to decide whether to call `RandomServoFieldsValid` (only when `isServo`) and for `SpanStart.isServo`.
     - **Servo with no usable range:** if `isServo` and the snapshot's `maxPos <= minPos`, reject it, logging "Maestro %d ch %d has no usable range (min %d us >= max %d us), fix the channel config". Without this, `PercentFromPosition` reads 0 and the travel stretch is disabled. The span would do nothing visible, and nothing would say why. The server UI and `parseServoConfig` both accept min ≥ max. (Amended 2026-10-22, from the T-008 pre-push review.)
     - **Config mismatches, both directions:**
       - If the snapshot says digital but any servo field after `maxGapMs` is non-zero, `ESP_LOGW` a config mismatch (the server thinks it's a servo), and still run the span as a toggle.
       - If the snapshot says servo and `minPos`, `maxPos`, `minMove` and `maxMove` are all 0, log a config mismatch ("the server sent a digital span for servo channel …; re-sync the controller config") rather than "invalid servo fields". (Amended 2026-10-22, pre-push review.)
     - A servo's `initialPct` is `PercentFromPosition(min, max, lastPos, inverted)`, and its `rangeUs` is `maxPos − minPos`.
     - A digital channel's `initialLevel` is `lastPos >= 1500`.
   - For type 7: `GpioMod.GetLevel`. The `SpanKey` for a GPIO span is `{GPIO_RANDOM, 0, channel}`: `module` is always 0 for GPIO.
   - **A span of `RandomSpanTable::END_GUARD_MS` or less is rejected with its own reason**: "%d ms is within the %d ms end guard, no change can run". The table would accept it and never change anything. (Jeff, 2026-10-22: T-009 logs it; § C is unchanged.)
   - Build the `SpanStart` with designated initializers, or `SpanStart s{}` followed by named assignments. Never use positional braces: eight adjacent `int`s transpose silently, and `Start` copies the other kind's fields too. (T-008 pre-push type review.)
3. **Under the table lock: one pass.** Take `randomTableMutex` (bounded 100 ms).
   - **On a timeout:** `ESP_LOGW` "random_effects_task: table mutex timeout, pass skipped". Free and log any message in hand as dropped, and wait 1 tick.
   - **Otherwise,** sample `now = esp_timer_get_time() / 1000` **after the take**, which is after the snapshot's mutex waits. Use that one value for everything in this hold:
     a. **If a message is in hand:**
        - Reject it if it was dispatched at or before the last clear: `(int32_t)(dispatchTickMs − lastClearMs) <= 0`.
        - Reject it if it arrived too late: `(int32_t)(now − (dispatchTickMs + spanMs)) >= −END_GUARD_MS`. Log how late (`now − dispatchTickMs`). `Start` would accept such a span, and `Collect` would drop it with no change and no log.
        - Otherwise `Start` it, with `startMs = dispatchTickMs`.
        - If `Start` returns false and `ActiveCount() == RANDOM_TABLE_CAPACITY`, run `Collect(now, changes)` first, because a slot may hold a span that is already inside its end guard. Then retry `Start` once.
        - A final false logs "table full (n/32 active)" when the table is full, and "gap rules rejected" otherwise. That would mean the table's copy of the gap rules disagrees with the parser's; T-008's `TableAcceptsExactlyWhatTheParserAccepts` pins them together.
        - On success, `ESP_LOGI` the key, span, gaps and (for a servo) speed/accel, so "it only moved once" can be diagnosed.
     b. **`Collect(now, changes)`,** then post each change as a malloc'd string with a **0 ms** send timeout. A failed send frees the string. Log the first dropped change per span, naming the queue and noting that the next move is sized from the dropped target. Track this with a per-`SpanKey` flag, reset on a successful `Start`; `Collect` gives no "span ended" signal, so a per-span total can't be logged.
     c. **The next wait:** `waitTicks` from `min(MsUntilNext(now), 250)`, rounded up. It is computed here so `MsUntilNext` is read under the lock, because panic and reload call `Clear()` from other tasks.
   - Give the mutex back, and free `data`.
   - **Why one `now`, sampled after the snapshot:** the snapshot can block for up to about 1 s on `maestroModulesMutex`. With a `now` from before it, `Collect` could emit a change after a span's deadline, so a random move would override the end-time event (§ C). With two different `now` values, a span could pass the late check and then be dropped by `Collect` with no log. (Amended 2026-10-22, pre-push review.)
   - **Any failure** → `ESP_LOGW` with the reason and the raw command. The failures are:
     - parse (logged with `RandomParseErrorName(error)`)
     - unknown module or channel
     - disabled channel
     - no usable channel range
     - config mismatch
     - invalid servo fields
     - span within the end guard
     - arrived after its end
     - dispatched before a clear
     - table full, or gap rules rejected

   | Change | String | Queue |
   |---|---|---|
   | servo | `1|0|<module>|<channel>|<targetPct>|<speed>|<accel>` | `servoQueue`, `message_id` = module idx (same as dispatch) |
   | digital Maestro | `1|0|<module>|<channel>|<2500 or 500>|0|0` | `servoQueue` |
   | GPIO | `5|0|<channel>|<1 or 0>` | `gpioQueue` |

**`randomTableMutex`** is created in `app_main`. It is the only lock on the table.

**`handlePanicStop`:**

- New step 1b, right after `AnimationCtrl.panicStop()`: take `randomTableMutex` (bounded 1 s), `Clear()`, record `lastClearMs = esp_timer_get_time() / 1000`, drain `randomQueue` (freeing each `data`), and give the mutex back.
  - Why the drain and the timestamp: spans still waiting in `randomQueue`, or one the random task is already processing, would otherwise `Start` after the clear, and random motion would follow a panic. Draining covers the queue. The `lastClearMs` check covers the message in hand (added 2026-10-22, from the T-007 review).
- On a timeout, `ESP_LOGE` and continue the panic.
- Padawans reach this through the existing ESP-NOW panic relay.

**`SERVICE_COMMAND::RELOAD_CONFIG`:** clear the table the same way (clear, record `lastClearMs`, drain `randomQueue`), before `loadMaestroConfigs()`.

### Modules

**`lib/Modules` `MaestroModule`:**

- Add `bool GetChannelSnapshot(int channel, MaestroChannelSnapshot *out)`, read under `stateMutex`.
- It returns `{ enabled, isServo, minPos, maxPos, home, inverted, lastPos }`, and false when out of bounds.
- Module lookup goes through `maestroModules` under `maestroModulesMutex` (bounded 1 s), the same as `servoQueueTask`.
- **`HomeServos`' digital branch records `lastPos = moves[i].ms`** after a successful send. Today only the servo branch does. After boot or a reload, an inverted digital channel outputs 2500 while its `lastPos` still reads its configured home, so `initialLevel` would be low and the first toggle would re-send the current level. This is harmless for the digital path, because `QueueCommand` sends no pre-position frame there. (Amended 2026-10-22, T-008 pre-push review.)

**`lib/Modules` `GpioModule`:**

- Track the last level set per channel, in `SendCommand` and `DefaultGpios`.
- Add `bool GetLevel(int channel, bool *out)`. Its bounds check is `channel < 0 || channel >= size` → false. Do **not** copy the existing off-by-one in `SendCommand`.

### Other

- **No `VERSION` change:** develop is already at 1.3.0, the release that will carry this.
- **QA plan** `.docs/qa/random-span-events.md`: add the bench cases in Verification.

## Acceptance criteria

- [ ] Both boards build, and `pio test -e test` is green (no regressions).
- [ ] Bench cases 1–8 and 10 below pass on the master, and case 9 passes on a padawan.
- [ ] No high-water-mark or task-watchdog warning for `random_effects_task` or `animation_dispatch_task` during case 3 at a 100 ms min gap for 60 s.
- [ ] Panic mid-span (case 6) shows no servo movement after the panic log line.
- [ ] `.docs/qa/random-span-events.md` has the bench cases with expected results.

## Out of scope

- Server-side `MINIMUM_FIRMWARE_VERSION` (AstrOs.Server Phase C).
- Manual commands cancelling spans: explicitly not wanted.
- Panic reaching PCA9685 servos (existing Backlog).
- Fixing `GpioModule::SendCommand`'s off-by-one bounds check (separate quick fix).
- Hardening existing `std::stoi` parsers (Backlog).

## Verification

- `pio test -e test` → green. `pio run -e lolin_d32_pro` and `pio run -e metro_s3` → both build.
- **Bench.** Run the server with AstrOs.Server T-007 and T-008. Spans are inserted by hand with the SQL in the server QA plan until the scripter UI lands. Monitor with `pio device monitor -e metro_s3`.
  1. **GPIO flicker.** GPIO span of 10 s, gaps 50–400 ms, on an LED. Expected: irregular blinking for 10 s, then it holds. An end-time "low" event turns it off.
  2. **Maestro digital flicker.** The same on a Maestro channel set to `isServo = false`. Expected: the same.
  3. **Servo twitch.** Span 35–65 %, move 2–8 %, gaps 200–800 ms, speed 0. Expected: small, irregular twitches that never leave the range.
  4. **Panel flap with end event.** Span 0–100 %, move 30 %–none, gaps 400–2000 ms, then a Home event at the end time. Expected: random flapping, then home exactly at the end and no random move after.
  5. **Slow servo.** Speed 5, gaps 100–200 ms, move 20 %. Expected: each move visibly completes before the next starts (travel-stretched gaps).
     - **5b. Accelerated short moves** (added 2026-10-22, from the T-008 distance-aware travel amendment). Speed 0, accel 3, move 5–10 %, gaps 100–200 ms. Expected: each move still completes before the next starts. A 10 % move on a 2000 µs channel models at about 925 ms, far past the drawn gap. At accel 0 the old linear scaling was exact, so case 5 alone wouldn't show the difference.
  6. **Panic mid-span.** Run cases 1 and 3 together, then panic. Expected: all random activity stops immediately, and the servos de-energize.
     - Also: a script with several spans at the same start time, panicked within ~10 ms of that start (while spans may still be queued). Expected: no random move after the panic log line.
  7. **Reload mid-span.** Deploy config during a span. Expected: the span stops, and config applies.
  8. **Table full.** A script with 33 concurrent spans. Expected: the 33rd logs "table full", and the others run.
  9. **Padawan.** Run cases 1 and 3 on a dome padawan's channels, then panic from the server. Expected: the same as cases 1, 3 and 6, with the panic arriving through the ESP-NOW relay.
  10. **Channel with no usable range.** Set a servo channel's min and max to the same µs, deploy the config, then run a servo span on it. Expected: the span is rejected with "no usable range … fix the channel config", and nothing moves. Restore the channel afterwards.

  Malformed commands aren't bench-tested. The server never sends one (its validation skips invalid spans), and T-007's native tests cover the firmware rejects.

## Implementation checklist

- [ ] Pure helpers in `lib_native/AstrOsRandomEffects` (`RandomSpanAdmission.hpp/.cpp`, see Rulings): `MaestroChannelSnapshot`, `BuildMaestroSpan` / `BuildGpioSpan`, `SpanArrivedTooLate`, `ClearFence`, `FormatSpanChange`, `DropLogThrottle`, `WaitTicksCeil`. RED first: `astros_random_span_admission_tests.cpp` fails to compile, then fails on its assertions against stubs; GREEN
- [ ] `MaestroModule::GetChannelSnapshot`; `HomeServos`' digital branch records `lastPos`
- [ ] `GpioModule` per-channel level tracking and `GetLevel`
- [ ] `src/main.cpp`: `randomQueue` + `queue_random_msg_t`, `randomTableMutex`, dispatch cases for types 6/7, the `default:` WARN
- [ ] `src/main.cpp`: `randomEffectsTask`
- [ ] `src/main.cpp`: panic step 1b and the `RELOAD_CONFIG` clear
- [ ] Purity grep clean; clang-format clean; README / CLAUDE.md lib-table text
- [ ] Full `pio test -e test` green; `pio run -e lolin_d32_pro` and `pio run -e metro_s3` build
- [ ] `.docs/qa/random-span-events.md` bench cases
- [ ] Code review (fresh context); address Critical/Important
- [ ] Bench (human-gated): cases 1–8 and 10 on the master, case 9 on a padawan

## Failure-mode inventory

- **§1 External calls / sends:**

  | Call | Condition | Response |
  |---|---|---|
  | `xQueueSend(randomQueue, …, 100 ms)` | queue full | free the copy, `ESP_LOGW`; the span is lost, and the script timing is kept |
  | change send to `servoQueue` / `gpioQueue` (0 ms) | queue full | free; log the first drop per span (per-`SpanKey` flag); the next change retries naturally, sized from the dropped target |
  | `randomTableMutex` in the task (100 ms) | timeout | `ESP_LOGW`, skip the pass; free and log any message in hand as dropped; retry after 1 tick |
  | `malloc` | NULL | log, skip the change or span |
  | `maestroModulesMutex` (1 s) | timeout | `ESP_LOGW`, reject the span |
  | `randomTableMutex` in panic (1 s) | timeout | `ESP_LOGE`, continue the panic; the servo offs still go out, and a stray random move can follow (logged) |

- **§2 Crash recovery.** N/A. The span table is RAM only, and a reboot loses it, which is correct.
- **§3 Concurrency:**
  - **The table:** `randomEffectsTask` is the writer, plus `handlePanicStop` and `RELOAD_CONFIG`. It is synchronized by `randomTableMutex`. Collect and post happen while holding it, so a panic that has taken it guarantees no post after `Clear()`.
  - **Queued and in-flight spans:** the clear drains `randomQueue` and records `lastClearMs` under the same mutex. A span the random task already holds is rejected at `Start` by the `lastClearMs` check, so nothing dispatched before a panic can start after it.
  - **Mutex ordering:** the random task never holds `randomTableMutex` while taking `maestroModulesMutex`. The snapshot is taken before `Start`, so there is no inversion with `servoQueueTask` or panic.
  - **Every table call happens under `randomTableMutex`**, including `MsUntilNext`, which computes the next wait at the end of the hold. One `now` is sampled after the take and used for the whole hold.
  - **Channel state reads** go under each module's `stateMutex`, per T-003.
- **§4 Cross-platform.** Both boards. `esp_random` is hardware RNG with Wi-Fi up (ESP-NOW is always up).
- **§5 Pre-existing state.** A span arriving while another span on the same channel is active → it replaces it (per contract). A span for a channel disabled in config → rejected with a log.
- **§6 Hostile or malformed input.** Script strings come from the SD card or the server. Parse failures are rejected by T-007's results, and servo fields by `RandomServoFieldsValid`. No exception paths.
- **§7 Resource lifecycle:**

  | Resource | Created | Cleanup | If missed |
  |---|---|---|---|
  | dispatch copy | dispatch | random task after parse; dispatch on a failed send | heap leak per span (test by watching heap over case 3) |
  | change string | random task | the servo/gpio consumer; the random task on a failed send | heap leak per change |
  | table entries | `Start` | deadline / `Clear` | none, fixed capacity |

## Rulings (2026-10-30, during implementation)

- **Pure pieces live in `lib_native/AstrOsRandomEffects`** (`RandomSpanAdmission.hpp`): span building and the snapshot checks, change formatting, the too-late check, the clear fence, the dropped-change log throttle, and the ms → ticks round-up. The Task section describes this logic in `main.cpp` without pinning where it lives. CLAUDE.md's default is to put logic that can be native-tested in a PURE lib, and `main.cpp` keeps the FreeRTOS loop, locks and queues.
- **The span table is file-static in `main.cpp`**, not a local of `randomEffectsTask`, because panic and `RELOAD_CONFIG` call `Clear()` from other tasks. It is still only touched under `randomTableMutex`.
- **`dispatchTickMs` is stamped before `AnimationCtrl.getNextCommandPtr()`.** The fetch and `panicStop()` serialize on `animationMutex`, so a stamp taken before the fetch always precedes the clear. With a stamp taken after the fetch, a command fetched just before the halt could be preempted by the panic (priority 10 against 5, same core), stamped after the clear, and started after the panic.
- **`lastClearMs` is a `ClearFence`:** a flag plus the clear's time, forgotten 2^30 ms (~12 days) after the clear and checked every pass. The literal `(int32_t)(dispatchTickMs − lastClearMs) <= 0`, with `lastClearMs = 0` at boot, rejects every span once uptime passes ~24.8 days without a clear.
- **The `NONE` case keeps its existing INFO line.** "Stay silent" is read as "does not reach the new unknown-type WARN".
- **`GetChannelSnapshot` also returns false on a `stateMutex` timeout** (a reload holds `stateMutex` across the SD read), logging the timeout itself with the module's identity.
