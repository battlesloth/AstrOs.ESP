# T-010: Release each Maestro servo channel on its own minimum time

<!-- File: .docs/tasks/T-010-per-channel-release.md. Branch: feature/T-010-per-channel-release. PR title: "T-010: Release each Maestro servo channel on its own minimum time". -->

## Context

- **Spec:** AstrOs.Server `.docs/specs/20261122-0847-per-channel-servo-release-design.md` (approved by Jeff, 2026-11-22), § Firmware.
- **Wire format:** `.docs/protocol.md` § D.
- **Project:** PLAN.md § Per-channel servo release time.

**Why.** Today `CheckServos` powers a servo channel off at `max(5 s, WorstCaseTravelMs + 10 %)` (T-005). A linear actuator driven at speed 0 models as near-instant, so it is cut after 5 s mid-stroke. The v1.2.1 notes promised per-channel timeouts for 1.3, and Jeff held v1.3.0 until this lands.

**Server side.** AstrOs.Server T-013 stores the setting, and T-014 sends it as the 8th field of each Maestro channel entry. T-014 also refuses config sync to firmware below 1.3.0, which drops 8-field entries.

## Contract (pinned — do not change)

- **§ D channel entry:** `id:enabled:isServo:minPos:maxPos:home:inverted:releaseMs`.
- **Parse rules:**
  - 7 or more fields are accepted, with 7 meaning `releaseMs = 5000`.
  - A bad 8th field (not a whole number in 1000–120000) keeps the channel with 5000 and logs a warning.
  - Fields after the 8th are ignored, and the channel is kept.
  - Fewer than 7 fields, or a bad required field, drops the entry, as today.
- **Release rule:** `deadline = max(releaseMs, travel + ceil(travel × 10 %))`, with `travel = WorstCaseTravelMs(speed, accel)`, unchanged.
- **`AstrOsServoUtils` (PURE):**
  - `int ServoReleaseDeadlineMs(int speed, int acceleration, int floorMs)` replaces the two-argument form.
  - `MAESTRO_RELEASE_FLOOR_MS` is replaced by `MAESTRO_RELEASE_DEFAULT_MS = 5000`, `MAESTRO_RELEASE_MIN_MS = 1000` and `MAESTRO_RELEASE_MAX_MS = 120000`.
  - `MAESTRO_RELEASE_MARGIN_PERCENT` stays 10.
- **`servo_channel`** (`lib_native/AstrOsUtility/src/AstrOsStructs.h`) gains `int releaseMs`. Every default-constructed channel reads `MAESTRO_RELEASE_DEFAULT_MS`, including `MaestroModule::channels[24] = {}` (a zero-initialized struct must not mean 0 ms).
- **The SD file is unchanged:** it holds the entry string exactly as received, and it is re-parsed at boot and on reload.
- **Unchanged:** panic, digital channels (never released), `MaestroChannelSnapshot` (random spans don't use the release time), `VERSION` (stays 1.3.0).

**Amended 2026-11-22 (Jeff, from the pre-push review):**
- **A reload releases what it no longer tracks.** `LoadConfig` records which servo channels were energized before the copy (the copy resets `on`), in `pendingReloadRelease`. `CheckServos` then sends an off to each one that homing didn't re-arm, whether the new config disabled it or homing failed: the next tick after loading ends, within ~300 ms (PR #66 review, below). Enabled digital channels are left alone, because homing set their level. The selection is the pure `ChannelsToReleaseAfterReload` (`AstrOsServoUtils`).
  - Before this, a channel a reload disabled while it was energized stayed powered until a power-cycle. T-010's holds of up to 120 s make that likelier.
- **`ServoReleaseDeadlineMs` clamps an out-of-range floor.** A floor outside 1000–120000 (for example 0 from a memset struct) uses the default.
  - The rule is still max(floor, travel + 10 %) for every value in range.

## Task

- **`parseServoConfig`** (`lib_native/AstrOsUtility/src/AstrOsFileUtils.hpp`):
  - Apply the parse rules.
  - Its max-id pass and its parse pass must agree on what counts as a valid entry: one shared per-entry helper.
  - It reports a defaulted release time through an injected `AstrOsLogger` (`lib_native/AstrOsLogging`). The parameter defaults to an empty logger, so native tests stay silent. The caller `AstrOsStorageManager::loadMaestroServos` passes `makeEspLogger()` (`lib/AstrOsUtility_ESP`).
  - The warning names the channel id and the bad value.
- **`ServoReleaseDeadlineMs`:**
  - Add the new signature and constants.
  - `MaestroModule::CheckServos` passes `channels[i].releaseMs`.
  - Update its doc comment ("a 5 s floor (T-005)"), plus any other comment that names the 5 s floor as fixed: `git grep -n "5 s floor\|MAESTRO_RELEASE_FLOOR"`.
- **Leave the commented-out legacy parser alone.** The block after `loadMaestroServos` in `AstOsStorageManager.cpp` is dead code.
- **QA plan `.docs/qa/maestro-servo-release.md`:** new cases per the spec:
  - 15 s channel
  - default channel
  - travel beyond the setting
  - 1.2.x → 1.3.0 OTA with 7-field SD files
  - padawan
  - bad 8th field

## Acceptance criteria

- [x] `test/test_native/astros_file_utils_tests.cpp` covers:
  - 7 fields → 5000
  - 8 fields `…:15000` → 15000
  - 8th field `abc`, `999` or `120001` → kept, 5000, with one warning through a test logger
  - 9 fields → kept, the 9th ignored
  - 6 fields → dropped
  - a mix of entries → max id and parsed set agree
- [x] `test/test_native/astros_servo_utils_tests.cpp` covers:
  - `ServoReleaseDeadlineMs(0, 0, 15000) == 15000`
  - `ServoReleaseDeadlineMs(5, 0, 5000) == 26400` (24000 travel + 10 %)
  - `ServoReleaseDeadlineMs(5, 0, 30000) == 30000`
  - the default constant is 5000
- [x] Mutation checks: dropping the 7-field default makes a test fail, and so does using the floor as a cap (`min`).
- [x] A default-constructed `servo_channel` and `MaestroModule::channels` read 5000; covered natively where possible.
- [x] `pio test -e test` green; `pio run -e lolin_d32_pro` and `pio run -e metro_s3` build.
- [ ] Bench (human-gated, joint with AstrOs.Server T-014): the new `maestro-servo-release.md` cases pass, including case 23 (amendment).

## Out of scope

- The server side (AstrOs.Server T-012–T-015).
- A "never release" setting (spec non-goal).
- Per-command log levels (T-011).
- PCA9685 servos (`ServoModule.cpp.dep`).

## Verification

- `pio test -e test` green; both boards build; clang-format clean on touched files.
- **Bench:** a server with T-014 (branch or develop). **Flash this build to every controller before the first sync** (spec § Constraints: `1.3.0-dev` builds without this task pass the gate and drop 8-field channels).
  - Set Right Pan's `release_ms` to 15000 by SQL and sync. The master logs `Turning off servo 1` ~15 s after a scripted move; ch6 at the default releases at ~5 s.
  - A speed-5 move on a channel set to 10000 releases at ~26.4 s.
  - Flash a board from v1.2.2 with its 7-field SD files: it boots and keeps its channels before any sync.
  - Repeat the 15 s case on the core padawan, log-only.

## Implementation checklist

- [x] RED → GREEN: `ServoReleaseDeadlineMs(speed, accel, floorMs)` tests in `astros_servo_utils_tests.cpp`; new signature + `MAESTRO_RELEASE_{DEFAULT,MIN,MAX}_MS`; existing two-argument tests moved to the default floor; `CheckServos` passes `channels[i].releaseMs`
- [x] RED → GREEN: `parseServoConfig` cases in `astros_file_utils_tests.cpp` (7 / 8 / bad 8th with a capturing test logger / 9 / 6 fields, mixed max id); `servo_channel.releaseMs` with the default; shared per-entry helper; `AstrOsLogger` parameter
- [x] `loadMaestroServos` passes `makeEspLogger()`; `MaestroModule::channels` default reads 5000 (native test on the same `{}` form)
- [x] Comment sweep: `git grep -n "5 s floor\|MAESTRO_RELEASE_FLOOR"`
- [x] Mutation checks: require 8 fields; no struct default; floor as a cap (`min`); no trailing-colon rule; no whitespace trim — each fails its tests
- [x] `pio test -e test` 623/623; both boards build; clang-format 18.1.8 clean on touched files (now 630/630 after the PR #66 fix)
- [x] QA plan `maestro-servo-release.md` cases 17–22; code review (see Review notes)
- [x] PR #66 review fix: the reload release moved into `CheckServos` via the pure `StepReloadRelease` (`astros_servo_utils_tests.cpp`, RED → GREEN; mutations caught: clearing a bit before its outcome, no loading gate, no filter); QA case 23 and the panic / script edge cases; 630/630; both boards build
- [x] PR #66 second review fix: `loading` is `std::atomic<bool>`; the reload release counts a channel as re-armed only when `isServo && on` (RED → GREEN; four filter mutations caught); 631/631; both boards build
- [ ] Bench (joint with AstrOs.Server T-014, human-gated)

## Review notes (2026-11-22)

Fresh-context review of the uncommitted diff. 0 Critical, 2 Important, both fixed.

- **Fixed (Important):** `makeEspLogger` forwarded to `esp_log_writev`, which "adds no formatting" (ESP-IDF 5.5 `esp_log_write.h`: it "does not add any formatting elements such as color, timestamp, or tag"). On target the warning would have printed with no `W (…) tag:` prefix and no newline, glued onto the next line. T-010 is its first caller.
  - **Ruling:** fix the adapter in `lib/AstrOsUtility_ESP`, although that lib isn't in the Task list. Each level now formats into a 256 B buffer and logs it through `ESP_LOGx`.
  - Why: the header promises "the normal ESP-IDF logging pipeline", and the task's warning can't be correct without it.
  - Not native-testable (ESP-IDF); bench case 22 checks it.
  - Cost if wrong: another caller relying on the raw form. There is none.
- **Fixed (Important):** an entry ending in `:` (an empty 8th field) now warns.
  - This overturns the earlier ruling that it parses as 7 fields silently: a server mapping bug that drops `releaseMs` emits `…:0:` (JS `join` turns `undefined` into `''`), which is the spec's "setting silently doesn't apply" anti-goal.
  - `splitServoEntry` restores the empty part `splitString` drops.
- **Fixed (minor):**
  - Trailing whitespace is trimmed, so a hand-edited file's final newline no longer rejects the last channel's release time; 1.2.x tolerated it.
  - The redundant `ch.releaseMs = DEFAULT` is removed (the struct default covers it).
  - A `static_assert` keeps the default inside the range.
  - The "zero-initialized" wording now says `{}`-initialized, not memset.
  - QA case 22 says reboot only (`RELOAD_CONFIG` comes only from a deploy, which rewrites the SD file).
  - Stale QA lines: the two-argument deadline, the `MAESTRO_RELEASE_FLOOR_MS` knob, and the intro. (The pre-push review found more fixed-floor phrasing; fixed there, see below.)
- **Ruling (minor 6):** the max-id pass and the parse pass share the structural check (`splitServoEntry`) but not the numeric one. A bad required field still counts toward the vector size and leaves a disabled default slot, exactly as 1.2.x did.
  - Why: moving the numeric parse into the max-id pass would change 1.2.x behavior for no user gain.
  - Cost if wrong: an entry with a bad required field and id ≥ 24 still makes `loadMaestroServos` reject the file, as before.
- **Superseded by the pre-push review:** release-time warnings were logged while `LoadConfig` holds `stateMutex`. That's ~8 ms per line, and only for a bad config on the boot or reload path, which already tolerates dropped commands.

## Pre-push review notes (2026-11-22)

`/pr-review-toolkit:review-pr` (five agents) found nothing Critical. The branch merges cleanly with T-011, and the merged tree builds both ways. Changes, each mutation-checked where it is code:

- **Jeff's amendments** (above): the reload-release path (case 23) and the floor clamp. Tested: `ChannelsToReleaseAfterReloadPicksEnergizedChannelsHomingDidNotRearm`, `ServoReleaseDeadlineMsClampsAnOutOfRangeFloorToTheDefault`.
- **Ruling: one summary warning per kind per parse, not one per channel.**
  - The kinds are bad release values (up to four listed, each cut to 20 characters), dropped entries, and entries with more than 8 fields.
  - Why: LoadConfig parses under `stateMutex`. A server bug emitting `…:0:` for all 24 channels would have meant 24 lines (~290 ms) under the lock, dropping script commands.
  - The exact texts are pinned by tests, and QA case 22 quotes the framed line.
  - Dropped entries and extra fields used to pass silently. A lost `|` that merges two entries now warns.
  - Cost if wrong: an operator sees "ch N 'v'" for at most four channels per line.
- **Ruling: one INFO line per config load,** `Maestro module M release times: ch1 15000 ms; others 5000 ms` (pure `FormatReleaseTimes`). It shows what a board applied, which a log-only padawan otherwise can't (case 21).
- **Smaller changes:**
  - `parseReleaseMs` returns `std::optional<int>`, and a blank entry after a final `|` is ignored.
  - A `static_assert` on `servo_channel{}.releaseMs`.
  - The adapter skips formatting for compiled-out levels.
- **New tests:** `…:0:0`, bad-8th-plus-9th, trailing space or tab, an over-long value, and the full warning texts.
- **QA:**
  - cases 18 and 19 say accel 0
  - case 18 is marked as the only per-channel discriminator
  - case 20 times from `Homing Servos`
  - case 17 names the RX line
  - a verbose-build precondition for after T-011
  - reset ch1 after case 19
  - the remaining fixed-floor phrasing, and the tuning-knob note (the server always sends field 8)
- **Docs:** the logger API comments (`AstrOsLogger.hpp`, `README`) now describe the formatting adapter. The `CheckServos` comment path and attribution are corrected, along with the "comes due together" claim.
- **Rejected:** a claim that `NvsManager.c` uses `servo_channel` in live C code. Lines 249–416 are inside a comment block.
- **Backlog** (pre-existing; ESP PLAN.md):
  - SD save and read don't check their errors (`fwrite`/`fclose`, `ferror`).
  - A testable `CheckServos` helper.
  - A CI guard for the T-011 grep.

## PR #66 review notes (2026-11-22, Copilot)

Two findings on the reload-release path (the amendment above), both verified against the locking and fixed in one change: **the release moved from `LoadConfig` into `CheckServos`.**

- **A stale-mask race.**
  - The problem: `releaseUntrackedServos` chose its channels under `stateMutex`, released it, and then took the send mutex. In between, a script command (`QueueCommand` has no `loading` gate) could re-arm a channel and start a move, and the stale off would then cut that move short while tracking said `on`.
  - The fix: `CheckServos` decides and enqueues under `stateMutex`, with a zero-wait send-mutex take, the same as its normal releases. A command can no longer land between the decision and the off.
- **A failed off enqueue left the servo powered.**
  - The problem: the config copy had already cleared `on`, so neither `CheckServos` nor `Panic` (for a newly disabled channel) would ever release it.
  - The fix: `pendingReloadRelease` keeps each bit until its off is queued. A failure counts in the existing `sendBusy` / `queueFull` / `noMemory` masks ("retry next tick") and retries every 300 ms tick.
- **Not while `loading`:** a tick between the copy and `HomeServos` would otherwise release servos that homing is about to re-arm.
- **Testing:** the per-tick step is the pure `StepReloadRelease` (`AstrOsServoUtils.hpp`), native-tested with an injected try-send: a bit clears only once its off is queued, failures retry, nothing goes out while loading. The send-mutex take and `setServoOff` wiring are on-target only: both boards build, and bench case 23 expects the `CheckServos: … left energized by a reload` line (it can't force an enqueue failure, though homing's frames may cause one QueueFull retry).

## PR #66 review notes, second round (2026-11-23, Copilot)

Two findings on the first round's fix (32f0bd5), both verified against the code and fixed.

- **`loading` was a plain `bool` read across tasks.**
  - The problem: `CheckServos` read it under `stateMutex`, but `LoadConfig` writes it with no lock, so the mutex ordered nothing. `SetServoPosition` had the same unlocked read before T-010.
  - The fix: `std::atomic<bool>`, as elsewhere in the repo (`AnimationController`, `GpioModule`, `OtaReceiver`).
  - Why not guard it with `stateMutex`: a 50 ms take can fail, and a clear that fails would leave `loading` stuck at true (slider moves dropped, pending offs held forever).
  - Severity: no failure was reachable in the built firmware. In the plain-`bool` object code the `true` store precedes the `stateMutex` section that sets the pending bits, and the `false` store follows the `HomeServos()` call. Nothing in the source guaranteed that order.
- **A disabled digital channel could lose its pending off.**
  - The problem: `QueueCommand` sets `on` on any channel, and the filter dropped a pending bit on `on` alone, because "its normal release takes over". `CheckServos` never releases a digital channel, so nothing took over.
  - How a once-energized servo gets there: GPIO then disabled in the UI before one sync, or an entry the parser dropped (a `{}` slot). Disabling a servo directly keeps `isServo`, so it doesn't. A script command must also land before the first tick after loading; one that was waiting on homing's send mutex does.
  - The fix: re-armed means `isServo && on` (`ChannelsToReleaseAfterReload`). An enabled digital channel is still exempt, and a disabled servo a command re-armed still releases on its own deadline.
  - Limit: a command that arrives after the off still drives a disabled digital channel until something else changes it. The command paths have never checked `enabled`. Backlog.
- **Backlog** (pre-existing, found while checking the second finding): the missing `enabled` check above, and a module removed from the config is destroyed without offs (deleting and re-adding one does this: the server's `idx` is autoincrement, so it comes back as a new module).
