# T-011: Log per-command lines only in verbose bench builds

<!-- File: .docs/tasks/T-011-verbose-command-logs.md. Branch: feature/T-011-verbose-command-logs. PR title: "T-011: Log per-command lines only in verbose bench builds". -->

## Context

**What the bench measured (2026-11-22, T-009 QA case 11).** The per-command INFO lines throttle the queue tasks.
- Under five 20–40 ms spans the console ran at ~81 % of 115200 baud.
- `servoQueueTask` drained one message per ~40 ms. `Queueing servo command` and `Setting servo …` are ~240 B (~21 ms) per servo command.
- `gpioQueueTask` drained one per ~20 ms.
- So a scripted servo command waited 420 ms behind the random half of `servoQueue`.
- The dispatch loop's own `… command val` lines also stretched a same-time burst of five span starts to ~290 ms.

Release builds log at INFO (`CONFIG_LOG_DEFAULT_LEVEL=3`), so a droid pays the same cost.

**The Backlog item.** PLAN.md Backlog "Per-command INFO logging slows the queue tasks". Jeff, 2026-11-22: do it before v1.3.0, behind a build flag.

**The catch.** `CONFIG_LOG_MAXIMUM_LEVEL=3` in both `sdkconfig`s compiles `ESP_LOGD` out entirely, and the bench QA plans read these exact lines. Hence a flag rather than a plain demotion.

## Contract (pinned — do not change)

**The macro.** `ASTROS_LOG_CMD(tag, format, ...)` in a new `lib/AstrOsUtility_ESP/include/AstrOsCommandLog.h`:
- `ESP_LOGI` when `ASTROS_VERBOSE_COMMANDS` is defined
- otherwise `ESP_LOGD`, which is compiled out at the current maximum level

**Exactly these lines move to it,** with their text unchanged (the QA plans match on it):
- `src/main.cpp`:
  - `Serial command val:` (~702)
  - `Maestro command val:` (~761)
  - `I2C command val:` (~783)
  - `GPIO command val:` (~805)
  - `Random span command val:` (~829)
- `lib/Modules/src/GpioModule.cpp`: `Sending Command =>` (~96)
- `lib/Modules/src/I2cModule.cpp`: `Sending Command =>` (~55)
- `lib/Modules/src/MaestroModule.cpp`:
  - `Queueing servo command =>` (~130)
  - `Setting servo …` (~200)

**Amended 2026-11-22 (Jeff, from the code review):**
- `src/main.cpp` `i2cQueueTask`'s `I2C Command received on queue =>` (~1559) is a tenth site. It was missed in authoring; its servo and GPIO siblings are already DEBUG.
- Boot logs one INFO line, `Verbose command logs: on|off` (after the version line), so a bench console shows which build it is.
- **From the pre-push review (Jeff): failure lines name their command.** With the per-command lines compiled out, they no longer carry the context those lines gave.
  - Dispatch: `Malloc … dispatch data fail` and `Send … queue fail` (serial with its channel, servo, i2c, gpio) append `dropped: %.64s` of the command.
  - Modules: the GPIO `invalid GPIO channel` / `GPIO channel %d not available` lines and the Maestro `Invalid channel %d on module %d` lines (script and slider) name the channel and the command.
  - `I2cModule::SendCommand` logs `I2C write to 0x%02X failed: <esp_err>`; it used to discard `write()`'s error.
  - The `GpioModule` off-by-one stays a separate Backlog quick fix.

**Stays INFO:** everything else, including `Random span started`, `Turning off servo`, panic, reload, heartbeat and `RAM left`.

**Builds:**
- **Release, RC and CI builds** don't define the flag. `platformio.ini` and both `sdkconfig`s are unchanged.
- **Bench builds** set it: `PLATFORMIO_BUILD_FLAGS="-D ASTROS_VERBOSE_COMMANDS" pio run -e metro_s3 -t upload`.

## Task

- The header and the ten call sites (nine, plus the amended I2C queue line).
- Correct the drain comment at `lib_native/AstrOsRandomEffects/include/RandomSpanAdmission.hpp:133` ("drain about one message per 10 ms tick"): the tick is the wait between messages, and the message's own work comes on top. Give this task's measured numbers for both build types.
- **Bench docs:**
  - **QA plan preconditions:** `random-span-events.md`, `maestro-servo-release.md` and `code-review-phase-c.md` say bench builds need the flag, and that without it the per-command lines don't appear.
  - **`random-span-events.md` case 11:** its bounds hold for verbose builds. Add the production (flag-off) numbers this task measures.
  - **CLAUDE.md:** where it describes monitoring, the same note.
- **Strike the PLAN.md Backlog item,** pointing to T-011.

## Acceptance criteria

- [x] The ten sites use `ASTROS_LOG_CMD`; `git grep -n 'ESP_LOGI(TAG, "\(Queueing servo command\|Setting servo\|Sending Command\|I2C Command received on queue\|[A-Za-z ]*command val\)' -- src lib ':!*.dep'` returns nothing. (Unscoped, it also matches `.docs/completed-plans/` and the retired, uncompiled `lib/Modules/src/ServoModule.cpp.dep`; both are left alone.)
- [x] Four builds pass: `lolin_d32_pro` and `metro_s3`, each with and without `-D ASTROS_VERBOSE_COMMANDS`.
- [x] On each board's `firmware.elf`, all nine distinct per-command strings (ten sites; GPIO and I2C share `Sending Command =>`) are absent flag-off and present flag-on, while `Turning off servo`, `Random span started` and `Verbose command logs` are present both ways (`strings -a … | grep -cF`; the loop is in Verification). Checked 2026-11-22, after the amendment.
- [x] `pio test -e test` green (611/611).
- [x] **Bench, verbose build:** the boot line reads `Verbose command logs: on`, and QA case 11 shows every line it names, with results as on 2026-11-22.
- [x] **Bench, production timing:** recorded in the task file and in case 11, with each number given as dispatch → execution (scripted servo, GPIO) and the same-time offset. Run case 11 three times on a flag-off build with a **throwaway instrumentation commit** that is never merged:
  - It logs at WARN only the dispatch line and the execution line of commands whose delay field is non-zero. In case 11 those are `5|2000|0|1`, `1|3000|1|1|70|0|0` and `5|10000|0|0`.
  - The console then stays near idle, so the timing matches a release build.
  - Each run's boot line reads `Verbose command logs: off`.
  - **Gate:** the scripted servo's dispatch → execution comes in clearly under the verbose 420 ms, at most 250 ms in every run, or the task reopens. Expected: servo ~200 ms, GPIO under 100 ms.
  - **Result (2026-11-22): the gate failed at 310 ms, and Jeff closed the task on the measured numbers** (see Bench results). The servo residual isn't logging: the serial-channel task sends one Maestro frame per tick. The drain moves to ESP T-012, before v1.3.0.
- [x] The comment, QA plans, CLAUDE.md and Backlog edits are done.

## Out of scope

- The console baud rate and `sdkconfig` log levels (options (b) and (c), not chosen).
- Other INFO lines.
- Changing the half-queue share (`RandomChangeHasRoom`).
- The same-time offset's root cause, relative dispatch timing (PLAN.md Backlog).

## Verification

- `pio test -e test`; the four builds; clang-format on touched files.
- The ELF check, on each board after a flag-off and a flag-on build:
  `for s in "Serial command val" "Maestro command val" "I2C command val" "GPIO command val" "Random span command val" "Sending Command =>" "Queueing servo command =>" "Setting servo %d on module" "I2C Command received on queue"; do strings -a .pio/build/<env>/firmware.elf | grep -cF "$s"; done`
  It prints 0 for each string flag-off and ≥ 1 flag-on. `Turning off servo`, `Random span started` and `Verbose command logs` must be ≥ 1 both ways.
- The two bench runs above, on the master with AstrOs.Server develop driving case 11 by `curl` (bench reference: `.docs/qa/random-span-events.md` § Bench preconditions / Measuring).

## Implementation checklist

- [x] `lib/AstrOsUtility_ESP/include/AstrOsCommandLog.h` with `ASTROS_LOG_CMD`; the ten sites switched (nine + the I2C queue line, amended) and the boot line (text unchanged); the acceptance grep returns nothing
- [x] Four builds (both boards × flag on/off) and `pio test -e test` 611/611; clang-format clean
- [x] `RandomSpanAdmission.hpp:133` drain comment corrected (numbers filled in after the bench)
- [x] QA plan preconditions (`random-span-events.md`, `maestro-servo-release.md`, `code-review-phase-c.md`), CLAUDE.md monitoring note, PLAN.md Backlog strike
- [x] Code review (see Review notes)
- [x] Bench, verbose build: case 11 as on 2026-11-22
- [x] Bench, production timing: flag-off build with a throwaway instrumentation commit (never merged), case 11 ×3; record the numbers in this file and in case 11

## Review notes (2026-11-22)

Fresh-context review of the uncommitted diff.
- **Confirmed from the built artifacts:**
  - `PLATFORMIO_BUILD_FLAGS` appends to each env's `build_flags`, so the pin defines stay (PlatformIO 6.2 `getraw`), and the define reaches `lib/` as well as `src/`.
  - With the flag off, all nine distinct strings (of the ten sites; GPIO and I2C share `Sending Command =>`) are absent from `firmware.elf`, while `Turning off servo` and `Random span started` remain. Re-run after the amendment (the I2C queue line), on both boards.
  - No file overrides `LOG_LOCAL_LEVEL`.
- **Important 1 (Contract gap; Jeff amended the Contract):** `i2cQueueTask`'s `I2C Command received on queue` becomes a tenth site. Jeff also added a boot line, `Verbose command logs: on|off`.
- **Important 2:** the executor's grep ruling was wrong. The unscoped grep also matches `.docs/completed-plans/`, so the acceptance grep is now scoped `-- src lib ':!*.dep'`.
- **Minors fixed:**
  - CLAUDE.md: the lib-table row, the Logging convention, the named QA plans, and "Sending Command (GPIO, I2C)"
  - the header comment
  - `code-review-phase-c.md`'s stale `ESP_LOGI` mention
  - "built with" in the QA plans
  - the include placement in `MaestroModule.cpp`
  - case 11's verbose-build qualifier
- **Not done:**
  - A Windows/PowerShell note for `PLATFORMIO_BUILD_FLAGS` (it persists for the shell session), since bench work is on Linux.
- **Backlog candidates from the review** (outside this task):
  - Production error lines (`invalid GPIO channel`, `Send servo queue fail`) lost the context the preceding INFO line gave. Consider naming the command in the error line itself.
  - `AstrOs RX Read %d bytes` is INFO per incoming server message.
  - The `GpioModule::SendCommand` off-by-one is already in the Backlog.

## Pre-push review notes (2026-11-22)

`/pr-review-toolkit:review-pr` (five agents, run together with T-010) found nothing Critical in T-011. The branch merges cleanly with T-010, and the merged tree builds both ways.

- **Jeff's amendment** (Contract, above): failure lines name their command; the I2C write error is logged.
- **Verification hardened:**
  - The ELF strings check is now a repeatable step in Verification, run on both boards after the amendment.
  - The production-timing run has a pass/fail gate (servo ≤ 250 ms).
  - Both bench runs check the boot line.
- **Docs:**
  - CLAUDE.md lists the tenth line and words the Logging convention as the rule for new lines, naming the deliberate INFO exceptions; it says "normal (flag-off) builds", not "release".
  - The header comment separates whole-loop time from logging cost, and names the dispatch loop.
  - The QA lists say `Sending Command` (GPIO, I2C) and include the I2C queue line.
  - `code-review-phase-c.md` scopes the flag to § 2.1.
- **Backlog** (close-out, ESP PLAN.md):
  - `NONE command queued` at INFO
  - `AstrOs RX Read …` at INFO per server message
  - a CI step for the acceptance grep
  - the bad-command crash evidence: existing `std::stoi` parsers, already in the Backlog
  - `i2cQueueTask` silently drops unknown `message_id`s

## Bench results (2026-11-22)

Master (metro_s3), AstrOs.Server bench API, QA case 11 (`s1795413Qan`) driven by `curl`.

| Run | Build | Scripted GPIO | Scripted servo | Same-time offset | Queue fails | Half-full lines |
|---|---|---|---|---|---|---|
| 1 | verbose (`403aa2a`), boot line `on` | 70 / 70 ms | **410 ms**; Home 170 ms | +230–270 ms | 0 | 5 (one per span) |
| 2a | flag-off + throwaway instrumentation (`5b5f257`, `tmp/t011-instrumentation`, never merged), boot line `off` | 10 / 10 ms | **310 ms** | +40 ms | 0 | 2 (servo) |
| 2b | same | 10 / 10 ms | **310 ms** | +50 ms | 0 | 2 |
| 2c | same | 10 / 20 ms | **310 ms** | +40 ms | 0 | 2 |

- **The verbose build reproduces the 2026-11-22 numbers,** within the corrected case 11 bounds (GPIO ≤ 120, servo ≤ 450).
- **In a normal build the GPIO queue keeps up** (no GPIO half-full drop), and the same-time burst offset falls about 6×. That confirms what logging cost.
- **The servo gate (≤ 250 ms) failed: 310 ms in every run.**
  - The cause: `serialCh1/2QueueTask` handles one 4-byte Maestro frame per 10 ms tick, and every servo or digital change is three frames (speed, accel, target), so ~30 ms per command whatever the console does. Ten queued random changes give ~310 ms.
  - The Maestro link is 115200 baud (~1.4 ms for the bytes), so the UART is not the limit.
  - **Ruling (Jeff):** pass on the measured numbers, and drain the serial-channel queue per tick in a new task, ESP T-012, before v1.3.0. The estimated gate assumed logging was about half the servo cost; it was nearer a quarter.
- **After the runs, the master was restored to T-011's normal build** (the ELF has no instrumentation strings).
