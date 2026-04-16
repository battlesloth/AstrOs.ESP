# lib_native/ Restructure + AnimationCommand Extraction

## Context

AstrOs.ESP has 4 PURE libs (`AstrOsMessaging`, `AstrOsUtility`, `AstrOsLogging`, `AstrOsSerialProtocol`) mixed in with 10+ MIXED/HARDWARE libs under `lib/`. The flat directory makes it hard for new contributors to see which libs are native-testable. The user wants physical separation via a `lib_native/` directory so the PURE/MIXED distinction is immediately visible.

Paired with the restructure, the AnimationController's command parsing classes (`AnimationCommand`, `BaseCommand`, `SerialCommand`, `I2cCommand`, `GpioCommand`, `MaestroCommand`, `CommandTemplate`) are pure computation that should be extracted into a new native-testable lib — the next step in the extraction pattern piloted on `AstrOsSerialProtocol`.

## Design

### Part A — `lib_native/` infrastructure

**Move 4 existing PURE libs:**
```
lib/AstrOsMessaging        → lib_native/AstrOsMessaging
lib/AstrOsUtility          → lib_native/AstrOsUtility
lib/AstrOsLogging          → lib_native/AstrOsLogging
lib/AstrOsSerialProtocol   → lib_native/AstrOsSerialProtocol
```

**PlatformIO config** — add `lib_extra_dirs` so all envs scan both directories:
```ini
[env]
lib_extra_dirs = lib_native

; Each board env and [env:test] inherit this via [env].
; PIO already scans lib/ by default, so lib_extra_dirs adds lib_native/.
```

**CI purity guard** — update `PURE_LIBS` paths in `.github/workflows/pr-validation.yml` from `lib/...` to `lib_native/...`.

**Git** — use `git mv` so history follows the files.

### Part B — AnimationCommand extraction

**New lib:** `lib_native/AstrOsAnimationCommands`

**Files to extract** from `lib/AnimationController/src/` (note: headers are in `src/`, not `include/`):
- `AnimationCommand.hpp` / `AnimationCommand.cpp` — parses pipe-delimited command string into MODULE_TYPE, duration, module index; produces `CommandTemplate`
- `AnimationCommon.hpp` — `str_vec_t` typedef + `KangarooAction` enum
- `BaseCommand.hpp` / `BaseCommand.cpp` — pipe-delimiter `SplitTemplate()` utility
- `SerialCommand.hpp` / `SerialCommand.cpp` — Kangaroo serial protocol conversion
- `I2cCommand.hpp` / `I2cCommand.cpp` — I2C channel/value extraction
- `GpioCommand.hpp` / `GpioCommand.cpp` — GPIO channel/state extraction
- `MaestroCommand.hpp` / `MaestroCommand.cpp` — servo channel/position/speed/accel extraction

**New lib structure:**
```
lib_native/AstrOsAnimationCommands/
├── README                       (purity rule)
├── include/
│   ├── AnimationCommand.hpp
│   ├── AnimationCommon.hpp
│   ├── BaseCommand.hpp
│   ├── SerialCommand.hpp
│   ├── I2cCommand.hpp
│   ├── GpioCommand.hpp
│   └── MaestroCommand.hpp
└── src/
    ├── AnimationCommand.cpp
    ├── BaseCommand.cpp
    ├── SerialCommand.cpp
    ├── I2cCommand.cpp
    ├── GpioCommand.cpp
    └── MaestroCommand.cpp
```

**ESP_LOGE removal:** All command constructors currently use `ESP_LOGE` for parse-error logging. Remove these — constructors set safe defaults (0, empty string) on parse failure. Malformed scripts are validated at deploy time by the server, so these are defensive-only paths.

**Dependencies:**
- `AstrOsUtility` (already PURE, moving to `lib_native/`) — for `AstrOsEnums.h` (`MODULE_TYPE`) and `AstrOsStringUtils::stringFormat()` (used by `SerialCommand::ToKangarooCommand()`)
- No other dependencies. No FreeRTOS, no ESP-IDF.

**Consumers that need include path updates:**
- `lib/AnimationController/` (hpp + cpp) — was using local `src/` includes, now uses lib dependency
- `lib/Modules/` (SerialModule, I2cModule, GpioModule, MaestroModule) — update `<AnimationCommands.hpp>` umbrella include
- `src/main.cpp` — includes `<AnimationCommand.hpp>`

The umbrella header `lib/AnimationController/include/AnimationCommands.hpp` either moves to the new lib or gets replaced with direct includes in consumers.

**CI purity guard** — add `lib_native/AstrOsAnimationCommands` to `PURE_LIBS`.

### Native tests

**New file:** `test/test_native/astros_animation_commands_tests.cpp`

Test cases:
- `AnimationCommand` parsing: pipe-delimited input → correct `commandType`, `duration`, `module` fields
- `CommandTemplate` construction via `GetCommandTemplatePtr()` → correct type/module/val
- `SerialCommand`: Kangaroo protocol `GetValue()` produces correct formatted string for each `KangarooAction`
- `I2cCommand`: channel + value extraction from template string
- `GpioCommand`: channel + bool state extraction
- `MaestroCommand`: channel + position + speed + acceleration extraction
- Edge cases: malformed templates (missing fields, wrong delimiters) → safe defaults (0, "")
- `BaseCommand::SplitTemplate()` utility — correct splitting on pipe delimiter

### CLAUDE.md updates

- Library layout table: update paths for moved libs (now `lib_native/...`)
- Add `AstrOsAnimationCommands` to the PURE lib list
- Update "Adding a new extracted (PURE) lib" section to reference `lib_native/`

## Task checklist

- [x] Task 1: Create `lib_native/` directory, `git mv` the 4 PURE libs there
- [x] Task 2: Update `platformio.ini` with `lib_extra_dirs = lib_native`
- [x] Task 3: Update CI purity guard paths in `pr-validation.yml`
- [x] Task 4: Verify existing tests + builds pass after the move (no new code, just paths)
- [x] Task 5: Extract command classes into `lib_native/AstrOsAnimationCommands` (new lib + README)
- [x] Task 6: Update `AnimationController` and `Modules` consumers for new include paths
- [x] Task 7: Add native tests for `AstrOsAnimationCommands`
- [x] Task 8: Update CLAUDE.md with new paths + lib classification

## Verification

1. `pio test -e test` — all existing 100 tests pass + new animation command tests (~15-20 new cases)
2. `pio run -e metro_s3` + `pio run -e lolin_d32_pro` — clean builds, no new warnings
3. CI purity guard: confirm `lib_native/` paths are checked (push throwaway `#include <esp_log.h>` to a PURE lib, verify job fails)
4. `git log --follow lib_native/AstrOsMessaging/` — verify history followed through the move

## Out of scope

- AnimationScriptParser / AnimationQueueManager extraction — follow-up phase (Tier 2)
- PeerRegistrationFSM extraction — follow-up phase (Tier 3)
- Modules/ extraction — hardware boundary, not extractable
- Any changes to animation controller logic or queue behavior
