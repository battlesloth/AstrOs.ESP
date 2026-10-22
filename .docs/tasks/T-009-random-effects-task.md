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

**`randomEffectsTask`** (stack 4096, priority 8, core 1) owns a `RandomSpanTable(32, esp_random)`. It loops:

1. **Wait.** It does `xQueueReceive(randomQueue, …, pdMS_TO_TICKS(min(MsUntilNext(now), 250)))`.
2. **On a message:**
   - Parse it with T-007, choosing the parser by `msg.type`.
   - For type 6:
     - Take **one** snapshot of the Maestro channel (below). Use it both to decide whether to call `RandomServoFieldsValid` (only when `isServo`) and for `SpanStart.isServo`.
     - If the snapshot says digital but any servo field after `maxGapMs` is non-zero, `ESP_LOGW` a config mismatch (the server thinks it's a servo) and still run the span as a toggle.
     - A servo's `initialPct` is `PercentFromPosition(min, max, lastPos, inverted)`, and its `rangeUs` is `maxPos − minPos`.
     - A digital channel's `initialLevel` is `lastPos >= 1500`.
   - For type 7: `GpioMod.GetLevel`. The `SpanKey` for a GPIO span is `{GPIO_RANDOM, 0, channel}`: `module` is always 0 for GPIO.
   - Then build a `SpanStart` with `startMs = dispatchTickMs`. Take `randomTableMutex`. **Reject the span if `(int32_t)(dispatchTickMs - lastClearMs) <= 0`**, i.e. it was dispatched at or before the last panic or reload clear. Otherwise call `Start`.
   - **Any failure** (parse, unknown module or channel, disabled channel, invalid servo fields, dispatched before a clear, or a full table) → `ESP_LOGW` with the reason and the raw command. Parse failures log `RandomParseErrorName(error)`.
   - Free `data`.
3. **Collect and post.** Take `randomTableMutex` (bounded 100 ms), `Collect(now)`, and post each change as a malloc'd string with a **0 ms** send timeout. A failed send frees the string and increments a counter that is logged once per span.

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

**`lib/Modules` `GpioModule`:**

- Track the last level set per channel, in `SendCommand` and `DefaultGpios`.
- Add `bool GetLevel(int channel, bool *out)`. Its bounds check is `channel < 0 || channel >= size` → false. Do **not** copy the existing off-by-one in `SendCommand`.

### Other

- **No `VERSION` change:** develop is already at 1.3.0, the release that will carry this.
- **QA plan** `.docs/qa/random-span-events.md`: add the bench cases in Verification.

## Acceptance criteria

- [ ] Both boards build, and `pio test -e test` is green (no regressions).
- [ ] Bench cases 1–8 below pass on the master, and case 9 passes on a padawan.
- [ ] No high-water-mark warning for `random_effects_task` or `animation_dispatch_task` during case 3 at a 100 ms min gap for 60 s.
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
  6. **Panic mid-span.** Run cases 1 and 3 together, then panic. Expected: all random activity stops immediately, and the servos de-energize.
     - Also: a script with several spans at the same start time, panicked within ~10 ms of that start (while spans may still be queued). Expected: no random move after the panic log line.
  7. **Reload mid-span.** Deploy config during a span. Expected: the span stops, and config applies.
  8. **Table full.** A script with 33 concurrent spans. Expected: the 33rd logs "table full", and the others run.
  9. **Padawan.** Run cases 1 and 3 on a dome padawan's channels, then panic from the server. Expected: the same as cases 1, 3 and 6, with the panic arriving through the ESP-NOW relay.

  Malformed commands aren't bench-tested. The server never sends one (its validation skips invalid spans), and T-007's native tests cover the firmware rejects.

## Failure-mode inventory

- **§1 External calls / sends:**

  | Call | Condition | Response |
  |---|---|---|
  | `xQueueSend(randomQueue, …, 100 ms)` | queue full | free the copy, `ESP_LOGW`; the span is lost, and the script timing is kept |
  | change send to `servoQueue` / `gpioQueue` (0 ms) | queue full | free, count, log once per span; the next change retries naturally |
  | `malloc` | NULL | log, skip the change or span |
  | `maestroModulesMutex` (1 s) | timeout | `ESP_LOGW`, reject the span |
  | `randomTableMutex` in panic (1 s) | timeout | `ESP_LOGE`, continue the panic; the servo offs still go out, and a stray random move can follow (logged) |

- **§2 Crash recovery.** N/A. The span table is RAM only, and a reboot loses it, which is correct.
- **§3 Concurrency:**
  - **The table:** `randomEffectsTask` is the writer, plus `handlePanicStop` and `RELOAD_CONFIG`. It is synchronized by `randomTableMutex`. Collect and post happen while holding it, so a panic that has taken it guarantees no post after `Clear()`.
  - **Queued and in-flight spans:** the clear drains `randomQueue` and records `lastClearMs` under the same mutex. A span the random task already holds is rejected at `Start` by the `lastClearMs` check, so nothing dispatched before a panic can start after it.
  - **Mutex ordering:** the random task never holds `randomTableMutex` while taking `maestroModulesMutex`. The snapshot is taken before `Start`, so there is no inversion with `servoQueueTask` or panic.
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
