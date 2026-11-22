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

- [ ] `test/test_native/astros_file_utils_tests.cpp` covers:
  - 7 fields → 5000
  - 8 fields `…:15000` → 15000
  - 8th field `abc`, `999` or `120001` → kept, 5000, with one warning through a test logger
  - 9 fields → kept, the 9th ignored
  - 6 fields → dropped
  - a mix of entries → max id and parsed set agree
- [ ] `test/test_native/astros_servo_utils_tests.cpp` covers:
  - `ServoReleaseDeadlineMs(0, 0, 15000) == 15000`
  - `ServoReleaseDeadlineMs(5, 0, 5000) == 26400` (24000 travel + 10 %)
  - `ServoReleaseDeadlineMs(5, 0, 30000) == 30000`
  - the default constant is 5000
- [ ] Mutation checks: dropping the 7-field default makes a test fail, and so does using the floor as a cap (`min`).
- [ ] A default-constructed `servo_channel` and `MaestroModule::channels` read 5000; covered natively where possible.
- [ ] `pio test -e test` green; `pio run -e lolin_d32_pro` and `pio run -e metro_s3` build.
- [ ] Bench (human-gated, joint with AstrOs.Server T-014): the new `maestro-servo-release.md` cases pass.

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
- [x] `pio test -e test` 623/623; both boards build; clang-format 18.1.8 clean on touched files
- [x] QA plan `maestro-servo-release.md` cases 17–22; code review (see Review notes)
- [ ] Bench (joint with AstrOs.Server T-014, human-gated)

## Review notes (2026-11-22)

Fresh-context review of the uncommitted diff. 0 Critical, 2 Important, both fixed.

- **Fixed (Important):** `makeEspLogger` forwarded to `esp_log_writev`, which "adds no formatting" (ESP-IDF 5.5 `esp_log_write.h`). On target the warning would have printed with no `W (…) tag:` prefix and no newline, glued onto the next line. T-010 is its first caller.
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
  - Stale QA lines: the two-argument deadline, the `MAESTRO_RELEASE_FLOOR_MS` knob, and the intro.
- **Ruling (minor 6):** the max-id pass and the parse pass share the structural check (`splitServoEntry`) but not the numeric one. A bad required field still counts toward the vector size and leaves a disabled default slot, exactly as 1.2.x did.
  - Why: moving the numeric parse into the max-id pass would change 1.2.x behavior for no user gain.
  - Cost if wrong: an entry with a bad required field and id ≥ 24 still makes `loadMaestroServos` reject the file, as before.
- **Accepted (minor 9):** release-time warnings are logged while `LoadConfig` holds `stateMutex`. That's ~8 ms per line, and only for a bad config on the reload path, which already tolerates dropped commands.
