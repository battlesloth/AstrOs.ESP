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

**Stays INFO:** everything else, including `Random span started`, `Turning off servo`, panic, reload, heartbeat and `RAM left`.

**Builds:**
- **Release, RC and CI builds** don't define the flag. `platformio.ini` and both `sdkconfig`s are unchanged.
- **Bench builds** set it: `PLATFORMIO_BUILD_FLAGS="-D ASTROS_VERBOSE_COMMANDS" pio run -e metro_s3 -t upload`.

## Task

- The header and the nine call sites.
- Correct the drain comment at `lib_native/AstrOsRandomEffects/include/RandomSpanAdmission.hpp:133` ("drain about one message per 10 ms tick"): the tick is the wait between messages, and the message's own work comes on top. Give this task's measured numbers for both build types.
- **Bench docs:**
  - **QA plan preconditions:** `random-span-events.md`, `maestro-servo-release.md` and `code-review-phase-c.md` say bench builds need the flag, and that without it the per-command lines don't appear.
  - **`random-span-events.md` case 11:** its bounds hold for verbose builds. Add the production (flag-off) numbers this task measures.
  - **CLAUDE.md:** where it describes monitoring, the same note.
- **Strike the PLAN.md Backlog item,** pointing to T-011.

## Acceptance criteria

- [ ] The nine sites use `ASTROS_LOG_CMD`; `git grep -n 'ESP_LOGI(TAG, "\(Queueing servo command\|Setting servo\|Sending Command\|[A-Za-z ]*command val\)'` returns nothing.
- [ ] Four builds pass: `lolin_d32_pro` and `metro_s3`, each with and without `-D ASTROS_VERBOSE_COMMANDS`.
- [ ] `pio test -e test` green.
- [ ] **Bench, verbose build:** QA case 11 shows every line it names, with results as on 2026-11-22.
- [ ] **Bench, production timing:** recorded in the task file and in case 11, with each number given as dispatch → execution (scripted servo, GPIO) and the same-time offset. Run case 11 three times on a flag-off build with a **throwaway instrumentation commit** that is never merged:
  - It logs at WARN only the dispatch line and the execution line of commands whose delay field is non-zero. In case 11 those are `5|2000|0|1`, `1|3000|1|1|70|0|0` and `5|10000|0|0`.
  - The console then stays near idle, so the timing matches a release build.
  - Expected (an estimate to check, not a gate): servo ~200 ms, GPIO under 100 ms.
- [ ] The comment, QA plans, CLAUDE.md and Backlog edits are done.

## Out of scope

- The console baud rate and `sdkconfig` log levels (options (b) and (c), not chosen).
- Other INFO lines.
- Changing the half-queue share (`RandomChangeHasRoom`).
- The same-time offset's root cause, relative dispatch timing (PLAN.md Backlog).

## Verification

- `pio test -e test`; the four builds; clang-format on touched files.
- The two bench runs above, on the master with AstrOs.Server develop driving case 11 by `curl` (bench reference: `.docs/qa/random-span-events.md` § Bench preconditions / Measuring).

## Implementation checklist

- [ ] `lib/AstrOsUtility_ESP/include/AstrOsCommandLog.h` with `ASTROS_LOG_CMD`; the nine sites switched (text unchanged); the acceptance grep returns nothing
- [ ] Four builds (both boards × flag on/off) and `pio test -e test`; clang-format on touched files
- [ ] `RandomSpanAdmission.hpp:133` drain comment corrected (numbers filled in after the bench)
- [ ] QA plan preconditions (`random-span-events.md`, `maestro-servo-release.md`, `code-review-phase-c.md`), CLAUDE.md monitoring note, PLAN.md Backlog strike
- [ ] Code review
- [ ] Bench, verbose build: case 11 as on 2026-11-22
- [ ] Bench, production timing: flag-off build with a throwaway instrumentation commit (never merged), case 11 ×3; record the numbers in this file and in case 11
