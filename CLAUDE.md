# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

AstrOs.ESP is ESP-IDF firmware (built via PlatformIO) for the AstrOs astromech animation and operating system — it runs on ESP32-class boards embedded in animatronic droids and coordinates servos, GPIO, I²C, OLED displays, and serial peripherals, with ESP-NOW mesh networking between nodes.

## Build / flash / test

PlatformIO is the canonical build driver (see `platformio.ini`). Three environments exist:

| Env | Target | Purpose |
|---|---|---|
| `lolin_d32_pro` | ESP32, 8 MB flash, `partition_8mb.csv` | Original board |
| `metro_s3` | Adafruit Metro ESP32-S3, 16 MB flash, `partition_16mb.csv` | Newer board |
| `test` | `platform = native`, googletest | Host-side unit tests |

Commands:

```bash
pio run -e metro_s3                  # build firmware for metro_s3
pio run -e metro_s3 -t upload        # build + flash
pio device monitor -e metro_s3       # serial monitor (115200, esp32_exception_decoder)

pio test -e test                     # run all native unit tests
pio test -e test -f test_native      # run a specific test folder
pio test -e test --filter "*servo*"  # run tests matching a pattern
```

Native tests require a host C++ toolchain. On Windows this is mingw-w64 via MSYS2 (see `README.md`); on Linux any recent g++ works. The `test` env pins `-std=gnu++2a`.

There is no separate linter configured — rely on the compiler warnings emitted during `pio run`. Per-board `sdkconfig.<env>` files are committed; do not edit `sdkconfig` directly, edit the board-specific one.

Adding a new board means adding a new `[env:...]` block *and* supplying **all pin defines** as `build_flags` (`TX_PIN_1`, `RX_PIN_1`, `SDA_PIN`, `SCL_PIN`, `GPIO_PIN_0..9`, `RESET_PIN`, …). Pin assignments are compile-time constants, not runtime config.

## Architecture

### The wiring-harness pattern

`src/main.cpp` is ~1500 lines but it is almost entirely plumbing. It:

1. Creates **9 FreeRTOS queues** (animation, service, interface-response, serialCh1, serialCh2, servo, i2c, gpio, espnow).
2. Spawns **11 tasks pinned to specific cores** — most I/O and control tasks on core 1, the ESP-NOW receive path and astros UART RX on core 0.
3. Creates **4 `esp_timer` timers** — polling (master node, 2 s), maintenance, animation tick, servo move.
4. Hands queue handles to the singletons in `lib/` and lets them talk to each other through the queues.

The actual behavior lives in `lib/`. When tracing a feature, start at `main.cpp` to find which queue/task handles the entry point, then jump into the relevant lib.

### Library layout (what each thing owns)

- **`lib/AstrOsMessaging`** — wire-format serializers/parsers for messages exchanged over serial and ESP-NOW. **Must stay free of ESP-IDF/FreeRTOS includes** (see its `README`) because it is unit-tested on the native host. Treat this as a hard rule: no `freertos/*`, no `esp_*`, no `driver/*` in this lib.
- **`lib/AstrOsEspNow`** — ESP-NOW mesh: peer registration, polling (master → padawans), fragmentation (respects the 250 B ESP-NOW payload limit via a 20 B header + 180 B payload scheme), callbacks for send/recv.
- **`lib/AstrOsSerialMsgHandler`** — owns the UART protocol handler that bridges the serial interface queue and the interface-response queue.
- **`lib/AnimationController`** — loads scripts, computes the next command, pushes commands onto the hardware queues. Uses a FreeRTOS mutex (`animationMutex`) around `scriptEvents`.
- **`lib/AstrOsStorageManager`** — NVS for config + FAT/SD for scripts. Exposes the `AstrOs_Storage` singleton. Peer configs, service config (name, master MAC, fingerprint), and controller fingerprint live here.
- **`lib/Modules`** — hardware abstractions: `SerialModule`, `I2cModule`, `GpioModule`, `MaestroModule`. Each one owns a queue-consumer loop for its hardware.
- **`lib/Pca9685`** — PCA9685 servo driver sitting on top of `I2cMaster`. Two boards expected at `0x40` and `0x41`.
- **`lib/I2cMaster`** — serialises bus access with a FreeRTOS semaphore (1000 ms timeout). Uses the legacy ESP-IDF I²C API.
- **`lib/AstrOsDisplay`** — SSD1306 OLED rendering. Talks to hardware by pushing `queue_msg_t` entries into the `i2cQueue`.
- **`lib/SoftwareSerial`, `lib/Uuid`, `lib/AstrOsUtility*`** — support libs.
- **`components/ssd1306`, `components/mdns`** — ESP-IDF components (not PlatformIO libs). Kept here rather than as `idf_component.yml` dependencies.

### Runtime roles: master vs padawan

Every node boots into one of two roles, set at runtime from storage:

- **Master** — one per deployment. Polls padawans every 2 s (the `pollingTimer`), owns the interface UART, runs serial channel 1 at the AstrOs interface baud (115200).
- **Padawan** — everything else. Serial channel 1 defaults to 9600.

The role flag is `isMasterNode` in `main.cpp`. Touching it or the role-specific init paths means thinking about both sides.

### Queue message ownership (important)

Queues carry POD structs (`queue_msg_t`, `queue_serial_msg_t`, `astros_interface_response_t`, …). Several of them contain **raw `malloc`'d pointers** (`uint8_t *data`, `char *originationMsgId`, etc.). The convention is:

- **Producer** `malloc`s, `memcpy`s, calls `xQueueSend`. If send fails, producer frees.
- **Consumer** task must `free()` the embedded pointers *after* processing the dequeued message.

An April 2026 code review (`.docs/code-review/code-review.md`) catalogs several places where consumers currently fail to free — when adding or modifying queue consumers, make sure the ownership handoff is explicit and matches that convention.

### Known-fragile areas

`.docs/code-review/code-review.md` is a comprehensive review with P0–P3 findings. Skim it before making changes in any of these areas:

- `main.cpp` timer callbacks (`pollingTimerCallback`, `animationTimerCallback`) — leak hazards + stack pressure.
- `AstrOsEspNow` peer list — no mutex protecting `peers` vector.
- `AnimationController` — some state fields read outside the mutex; `CommandTemplate *` returned by raw pointer with caller-owns-delete contract.
- `NvsManager.c` `setKeyId` — assumes peer index < 100.
- Globals in `main.cpp` (`displayTimeout`, `discoveryMode`, `isMasterNode`, `rank`, `maestroModules`) — accessed cross-core without synchronisation.

When touching these files, prefer fixes that also resolve the relevant review item over drive-by changes.

## Conventions worth knowing

- **String buffers**: the codebase mixes `malloc`/`free` (idiomatic for C buffer payloads) and `new`/`delete` (C++ objects like `MaestroModule`, `CommandTemplate`). Stick with the existing style of the file you're editing; don't cross the streams within one allocation path.
- **Exceptions are effectively disabled**. Avoid `try`/`catch` in new code. Prefer `strtol` + `errno` over `std::stoi`; an uncaught exception will take down the whole FreeRTOS task.
- **Blocking on `portMAX_DELAY`** is used in several places but is a known footgun — prefer `pdMS_TO_TICKS(...)` timeouts with a log-on-failure path for new code.
- **Task stack sizes** live in the `xTaskCreatePinnedToCore` calls in `main.cpp`. Every task has a high-water-mark check that warns at 500 bytes remaining — if you see that warning in logs, bump the stack rather than chasing the symptom.
- **Logging**: use `ESP_LOGI/W/E(TAG, ...)`. Each file defines its own `TAG`. `main.cpp` uses `AstrOsConstants::ModuleName`.

## Things that are *not* in the repo

- No CI config (no `.github/`, no `.gitlab-ci.yml`). Builds and tests are run locally.
- No Cursor / Copilot rules.
- No pre-commit hooks or formatters configured.
- No submodules (`.gitmodules` is empty).

## Planning (MANDATORY)

**NEVER write implementation code without a written, committed plan.** This is a hard rule, with the exceptions below.

> **Note on plan storage:** Claude Code's in-session plan mode writes ephemeral working drafts to `~/.claude/plans/<session>.md`. Those are scratch. Before leaving plan mode and starting implementation, copy the finalized plan to `.docs/plans/<YYYYMMDD-HHmm>-<name>.md` and commit it. The in-repo location is the source of truth.

### Quick feedback mode

No plan is required for small, minimally invasive changes made while reviewing a completed feature. Examples: logging tweaks, wording/copy changes, spacing fixes, typo corrections, and stack-size bumps driven by an observed high-water-mark warning. Just make the change directly. If a "quick" change starts growing in scope, stop and write a plan.

### Light plan mode

For medium-sized changes that are straightforward but span more than a trivial tweak — e.g., adding a new queue item, or a self-contained bug fix touching 2-3 files — use a light plan:

1. Write a brief plan (a short description + a checklist of 3-5 tasks) and save to `.docs/plans/` using the `YYYYMMDD-HHmm-feature-name.md` convention described in the full-plan section below.
2. **Commit the plan file before writing implementation code.**
3. Check off tasks as completed and commit updates.

Light plans skip the brainstorming skill and don't require a scope guard evaluation. If the plan grows beyond ~5 tasks or starts spanning many layers, escalate to a full plan.

### Full plan workflow

For larger features or work that spans multiple layers:

1. Brainstorm the feature with the user (using `superpowers:brainstorming`).
2. Write the plan using `superpowers:writing-plans` and save to `.docs/plans/` with a timestamped filename including time (e.g., `20260327-1500-feature-name.md` using `YYYYMMDD-HHmm` format).
3. The plan must include a checklist of discrete tasks with checkboxes (`- [ ]`). Each task should be small enough to complete and commit independently.
4. **Commit the plan file to the repo before writing any implementation code.** This ensures the plan survives crashes, context loss, or session restarts.
5. As each task is completed, update the plan file to check off the box (`- [x]`) and commit the update. This makes the plan the single source of truth for progress.
6. If a session is interrupted, the next session should read the plan file to determine what has been done and what remains.

### Scope guard — break up large work

During planning, evaluate the total scope. If a feature involves **more than ~8 discrete tasks** it is probably too large for a single plan. In that case:

- **Warn the user** that the work should be broken into phases.
- **Propose separate plan files** for each phase (e.g., `20260330-ota-upgrades-phase1-api.md`, `20260330-ota-upgrades-phase2-ui.md`).
- Each phase should be independently shippable and testable.
- Get user approval on the phasing before proceeding.

For firmware features that span multiple layers, break at natural queue/task seams rather than at arbitrary task counts. Typical phasing:

- **Phase 1** — wire format + native tests (`lib/AstrOsMessaging` + `test/test_native/`)
- **Phase 2** — queue producer/consumer wiring (`src/main.cpp`, `lib/Modules`)
- **Phase 3** — hardware integration + QA plan

Each phase should compile, ship, and be testable on its own.

## QA Test Plans

For each feature, create a manual QA test plan in `.docs/qa/` with a descriptive filename (e.g., `ota-upgrade.md`, `animation-queueing.md`). Each plan should include:

- **Preconditions**: required state/setup before testing
- **Step-by-step test cases**: numbered steps with specific user actions
- **Expected results**: what should happen after each step or group of steps
- **Edge cases / negative tests**: invalid inputs, error states, boundary conditions

QA plans should be committed alongside the feature work they cover.

For features that touch native-testable code (`lib/AstrOsMessaging`, pure utilities in `lib/AstrOsUtility`), add or extend native unit tests in `test/test_native/` as the first line of defense. QA plans cover the end-to-end hardware behavior on top of that — they don't replace native test coverage.

## Skills & Subagents

Use the following skills and subagents as part of the development workflow:

- **Brainstorming** (`superpowers:brainstorming`): Always brainstorm before building new features. Explore intent, requirements, and design before writing code.
- **Write Plan** (`superpowers:writing-plans`): Write a plan before any multi-step implementation. Save plans to `.docs/plans/`.
- **Execute Plan** (`superpowers:executing-plans`): Use to execute written implementation plans with review checkpoints.
- **TDD** (`superpowers:test-driven-development`): Applies to code that compiles under `[env:test]` — today that is primarily `lib/AstrOsMessaging` and pure utility code in `lib/AstrOsUtility`. When adding new logic that *could* live in a native-testable lib, default to putting it there so tests can cover it. For code that touches FreeRTOS, ESP-IDF drivers, or hardware, use QA plans instead.
- **Feature Dev** (`feature-dev:feature-dev`): Use for guided feature development with codebase understanding and architecture focus.
- **Debugging** (`superpowers:systematic-debugging`): Use systematic debugging for any bug, test failure, or unexpected behavior before proposing fixes.
- **Verification** (`superpowers:verification-before-completion`): Always verify before claiming work is done or creating a PR. Run tests and confirm output — evidence before assertions.
- **Code Review** (`superpowers:requesting-code-review`): Request a code review after completing significant features or before merging.
- **Parallel Agents** (`superpowers:dispatching-parallel-agents`): Use parallel agents for independent tasks that can be worked on without shared state or sequential dependencies.