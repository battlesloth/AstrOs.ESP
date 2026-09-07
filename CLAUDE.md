# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

AstrOs.ESP is ESP-IDF firmware (built via PlatformIO) for the AstrOs astromech animation and operating system — it runs on ESP32-class boards embedded in animatronic droids and coordinates servos, GPIO, I²C, OLED displays, and serial peripherals, with ESP-NOW mesh networking between nodes.

## Branching and merge workflow

- **`develop` is the integration branch.** Feature work branches off `develop` and merges back via pull request. Branch names carry the task ID: `feature/T-NNN-<slug>` (`ci/...` and `fix/...` prefixes keep working for their kinds of work). One task per branch. PR title: `T-NNN: <task title>`, with verification evidence in the body.
- **Doc-only carve-out:** changes limited to `CLAUDE.md`, `README.md`, `PLAN.md`, `.docs/`, or other prose files may be committed directly to `develop`. Anything build-affecting always rides a branch + PR.
- **`main` is reserved for release-candidate cuts.** Direct commits to `main` are forbidden — they bypass the RC build pipeline that fires on `push: branches: [main]` and would pollute the RC stream. Always integrate through PR.
- **`release/rel_X.Y` branches** are cut from `main` when a version is ready to ship. Bug fixes for a released line target the relevant `release/rel_*` branch; forward-port vs backport is decided case-by-case.
- **PR validation** (`.github/workflows/pr-validation.yml`) runs on pull requests targeting `main`, `develop`, or `release/rel_*`. All four checks — native unit tests, both-board build matrix, AstrOsMessaging native-purity guard, and clang-format on changed files — must pass before merge.
- **When using Claude Code to commit on this repo:** always confirm `git branch --show-current` is not `main` before committing. If it is, stop and switch to `develop` (or a feature branch) first.

### Release workflow

1. **RC builds** — every push to `main` triggers `.github/workflows/rc-build.yml`, which auto-tags `v<BASE>-RC.<N+1>` and publishes a GitHub Pre-release with six artifacts (3 per board × 2 boards).
2. **Cutting a release** — create a `release/rel_X.Y` branch from `main` and push. `.github/workflows/release-build.yml` auto-tags `vX.Y.0` and publishes a full GitHub Release with the same six artifacts.
3. **Patch releases** — push a bug-fix commit to `release/rel_X.Y`. The workflow auto-tags `vX.Y.<N+1>`.
4. **After cutting a release branch** — open a PR on `develop` that bumps the `VERSION` file to the next planned minor (e.g., `1.0.0` → `1.1.0`). This is a manual step, not automated.
5. **Cache warming** — `.github/workflows/cache-warm.yml` runs weekly (Monday 06:00 UTC) to keep the PlatformIO cache warm. Can also be triggered manually.

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

Each lib is classified **PURE** (no ESP-IDF/FreeRTOS/driver includes; compiles under `[env:test]`), **MIXED** (algorithmic logic plus ESP-IDF/FreeRTOS wiring), or **HARDWARE-ONLY** (driver code that can only build on-target). PURE libs live in `lib_native/`; everything else stays in `lib/`.

| Lib | Class | Summary |
|---|---|---|
| `lib_native/AstrOsMessaging` | PURE | Wire-format serializers/parsers for serial + ESP-NOW. |
| `lib_native/AstrOsSerialProtocol` | PURE | Decodes validated UART messages into `DecodedCommand` / `DecodeReject` records. |
| `lib_native/AstrOsUtility` | PURE | String, path (`AstrOsPathUtils`), servo, and file utilities. |
| `lib_native/AstrOsLogging` | PURE | `AstrOsLogger` fn-ptr struct for optional diagnostics injection into PURE libs. |
| `lib_native/AstrOsAnimationCommands` | PURE | Pipe-delimited command template parsers (`AnimationCommand`, `SerialCommand`, `I2cCommand`, `GpioCommand`, `MaestroCommand`). Used by both `AnimationController` and `Modules`. |
| `lib_native/AstrOsAnimationEngine` | PURE | Script parsing, script-ID circular queue (`ScriptQueue`), and event dispatch (`getNextCommand`). The pure orchestration logic behind `AnimationController`. |
| `lib_native/AstrOsEspNowProtocol` | PURE | Decodes validated ESP-NOW packets into `HandlerResult { InterfaceMessage, diagnostic }` records. Dispatches by packet type and enforces role gating on master-vs-padawan-only types; peer-state-entangled handlers (registration, poll) remain in `AstrOsEspNow`. |
| `lib_native/AstrOsEspNowPeers` | PURE | `PeerList` class wrapping the bounded peer vector with named operations (`add`, `contains`, `findByMac`, `resetPollCycle`, `markPollAckReceived`, `listUnacked`). Holds no locks — `AstrOsEspNow` wraps each call with `peersMutex`. Also owns the relocated `espnow_peer_t` struct (NVS wire format, shared with `NvsManager.c` via `espnow_peer.h`). |
| `lib/AstrOsUtility_ESP` | MIXED | ESP-side helpers — `logError`, `makeEspLogger`. |
| `lib/AstrOsSerialMsgHandler` | MIXED | Thin adapter: validates, calls `AstrOsSerialProtocol`, hands responses to the interface-response queue. |
| `lib/AstrOsEspNow` | MIXED | ESP-NOW mesh: peer registration, polling (master → padawans), fragmentation (respects the 250 B ESP-NOW payload limit via a 20 B header + 180 B payload scheme), callbacks for send/recv. Single-record packet decoding delegates to `AstrOsEspNowProtocol`. |
| `lib/AnimationController` | MIXED | Thin adapter: wraps `AstrOsAnimationEngine` with a FreeRTOS mutex (`animationMutex`) and atomic flags. Owns file I/O (script loading via `AstrOsStorageManager`) and the panic-stop safety contract. |
| `lib/AstrOsStorageManager` | MIXED | NVS for config + FAT/SD for scripts. Exposes `AstrOs_Storage` singleton. Peer configs, service config, controller fingerprint. |
| `lib/Modules` | MIXED | Hardware abstractions: `SerialModule`, `I2cModule`, `GpioModule`, `MaestroModule`. Each owns a queue-consumer loop. |
| `lib/AstrOsDisplay` | MIXED | SSD1306 OLED rendering. Pushes `queue_msg_t` entries into the `i2cQueue`. |
| `lib/Pca9685` | HARDWARE-ONLY | PCA9685 servo driver on top of `I2cMaster`. Two boards at `0x40`/`0x41`. |
| `lib/I2cMaster` | HARDWARE-ONLY | Serialises bus access with a FreeRTOS semaphore (1000 ms timeout). Legacy ESP-IDF I²C API. |
| `lib/SoftwareSerial`, `lib/Uuid` | HARDWARE-ONLY | Support libs. |
| `components/ssd1306`, `components/mdns` | — | ESP-IDF components (not PlatformIO libs). Kept here rather than as `idf_component.yml` dependencies. |

### Adding a new extracted (PURE) lib

When pulling pure logic out of a MIXED lib, follow this pattern (piloted on `AstrOsSerialProtocol`):

1. **Create the lib directory under `lib_native/`** with `include/`, `src/`, and a `README` stating the purity rule and listing the forbidden include prefixes. PURE libs always live in `lib_native/`, never in `lib/`. PlatformIO discovers them via the `lib_extra_dirs = lib_native` setting in `platformio.ini`.
2. **Register it with the CI purity guard** — append the `lib_native/` path to the `PURE_LIBS` array in `.github/workflows/pr-validation.yml` (`native-purity` job).
3. **Prefer rich return values over logger injection** — return a struct describing what happened (e.g., `{ commands, rejects }` or `{ valid, reason }`) and let the MIXED caller log at the boundary with `ESP_LOGW`/`ESP_LOGE`. Only reach for `lib_native/AstrOsLogging`'s `AstrOsLogger` struct when diagnostics truly must live next to the logic (tight loops, complex internal state).
4. **Add native tests** under `test/test_native/` — the `[env:test]` target auto-discovers them.

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
- `AnimationController` — some state fields read outside the mutex.
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

- No Cursor / Copilot rules.
- A pre-commit hook at `.githooks/pre-commit` auto-formats staged C/C++ files with clang-format. Activate once per clone: `git config core.hooksPath .githooks`. The hook is opt-in — it does nothing until `core.hooksPath` is set.
- No submodules (`.gitmodules` is empty).
- GitHub Actions CI lives under `.github/workflows/` — PR validation, RC builds, release builds, and weekly cache warming (see the Release workflow section; design history in `.docs/completed-plans/2026-04/20260411-0905-ci-pipeline-design.md`).

## Workflow (MANDATORY)

**NEVER write implementation code without a committed task file.** Quick-tier fixes are the only exception. Rationale and templates: [`.docs/agentic-workflow.md`](./.docs/agentic-workflow.md). Task IDs are per-repo (independent of AstrOs.Server's sequence).

> **Note on plan mode:** Claude Code's in-session plan mode writes ephemeral working drafts to `~/.claude/plans/<session>.md`. Those are scratch. The committed task file in `.docs/tasks/` is the source of truth.

### Session ritual

- **Open:** read `PLAN.md` (repo root) and state current status — active project, in-progress task, what's next — before touching code.
- **Close:** update the `PLAN.md` Status block; append a Log entry (dated header + short sub-bullets) for any completed task. Quick fixes stay out of the Log. A session that ends without this is not finished.
- `PLAN.md` is authoritative over agent memory: when they disagree, `PLAN.md` wins.

### Three tiers

- **Quick** — small, minimally invasive fixes during bench testing or feature review: logging tweaks, wording/copy changes, typo corrections, stack-size bumps driven by an observed high-water-mark warning. No artifact; fix directly on the active branch. If it grows, stop and promote to a task.
- **Task** — anything else that passes the five sizing rules as one unit. One file: `.docs/tasks/T-NNN-<slug>.md` from [`.docs/templates/task.md`](./.docs/templates/task.md).
- **Project** — work that fails the sizing rules. Run a seam-discovery session (`superpowers:brainstorming`) to split it into task files + a `PLAN.md` section before implementing anything. Split at the natural firmware seams — wire format + native tests, then queue producer/consumer wiring, then hardware integration + QA — so each task compiles and ships on its own.

### Task rules

- The task file's Context / Contract / Task / Acceptance criteria / Out of scope / Verification sections are written and **committed before implementation code**. The Implementation checklist is added when work starts; check off + commit as work proceeds.
- **Sizing rules** (all must hold, else split): one session with headroom; zero unmade architectural decisions; independently verifiable; pinned interfaces; a diff small enough that it will actually be read.
- Treat the **Contract** section as immutable — wire formats shared with AstrOs.Server, NVS layouts on already-flashed boards, queue-ownership rules, PURE-lib purity. If it seems wrong, stop and raise it — do not adapt it silently. If a needed contract doesn't exist, designing it is its own task.
- Respect **Out of scope**. Adjacent improvements — including tempting fixes from the code-review catalog in files the task touches — go in a note or the `PLAN.md` Backlog, not the diff, unless the task is about that finding.
- Never make an architectural decision mid-task. If one surfaces, stop, state the options, and wait.
- A task is done only when its **Verification** section runs green — never claim completion without running it. Done also includes updating the owning feature's QA plan in `.docs/qa/`, moving the task file to `.docs/tasks/completed/`, and flipping the `PLAN.md` checkbox.
- If the diff is ballooning past what the task implies, stop and propose a split.

## QA Test Plans

For each feature, create a manual QA test plan in `.docs/qa/` with a descriptive filename (e.g., `ota-upgrade.md`, `animation-queueing.md`). Each plan should include:

- **Preconditions**: required state/setup before testing
- **Step-by-step test cases**: numbered steps with specific user actions
- **Expected results**: what should happen after each step or group of steps
- **Edge cases / negative tests**: invalid inputs, error states, boundary conditions

QA plans should be committed alongside the feature work they cover. Completing a task includes updating the owning feature's QA plan (or creating it if the feature is new) — the feature plans are the living regression suite; there is no per-task QA intermediate.

For features that touch native-testable code (`lib/AstrOsMessaging`, pure utilities in `lib/AstrOsUtility`), add or extend native unit tests in `test/test_native/` as the first line of defense. QA plans cover the end-to-end hardware behavior on top of that — they don't replace native test coverage.

## Skills & Subagents

Use the following skills and subagents as part of the development workflow:

- **Brainstorming** (`superpowers:brainstorming`): Always brainstorm before building new features — this is also the vehicle for seam-discovery sessions that decompose project-tier work into tasks.
- **Write Plan** (`superpowers:writing-plans`): Use when authoring project-tier task sets. The output lands as task files in `.docs/tasks/` (template: `.docs/templates/task.md`), not standalone plan documents.
- **Execute Plan** (`superpowers:executing-plans`): Use to execute written implementation plans with review checkpoints.
- **TDD** (`superpowers:test-driven-development`): Applies to code that compiles under `[env:test]` — today that is primarily `lib/AstrOsMessaging` and pure utility code in `lib/AstrOsUtility`. When adding new logic that *could* live in a native-testable lib, default to putting it there so tests can cover it. For code that touches FreeRTOS, ESP-IDF drivers, or hardware, use QA plans instead.
- **Feature Dev** (`feature-dev:feature-dev`): Use for guided feature development with codebase understanding and architecture focus.
- **Debugging** (`superpowers:systematic-debugging`): Use systematic debugging for any bug, test failure, or unexpected behavior before proposing fixes.
- **Verification** (`superpowers:verification-before-completion`): Always verify before claiming work is done or creating a PR. Run tests and confirm output — evidence before assertions.
- **Code Review** (`superpowers:requesting-code-review`): Request a code review after completing significant features or before merging.
- **Parallel Agents** (`superpowers:dispatching-parallel-agents`): Use parallel agents for independent tasks that can be worked on without shared state or sequential dependencies.