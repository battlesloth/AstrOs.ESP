# Native-Testable Refactor: Foundation + AstrOsSerialMsgHandler Pilot

## Context

AstrOs.ESP currently has ~2 natively-testable libs (`lib/AstrOsMessaging`, `lib/AstrOsUtility`) and a pile of MIXED libs where algorithmic logic is tangled with ESP-IDF/FreeRTOS calls. The April 2026 code review (and the resulting Phase A/B/C work) has made it clear that several of those MIXED libs — protocol parsing, script event selection, peer registration FSM — would benefit from unit tests but can't be reached today because they pull in `esp_log.h`, FreeRTOS, or hardware drivers.

Goal: **establish a convention for extracting pure logic into sibling libs that are unit-tested under `[env:test]`**, then pilot the pattern on `AstrOsSerialMsgHandler` — the lowest-risk, highest-ROI target. Animation and dispatch hot paths (recently tuned in Phase C) are explicitly out of scope for this plan; their extraction will be follow-up work once the pattern is proven.

Two design principles guide the extraction:

1. **Rich return values are the primary error channel.** Pure libs should return structs/enums describing what happened (`{ valid, reason, type, parts... }`) so the ESP-side caller logs at the boundary. This is the pattern `AstrOsMessaging::validateSerialMsg` already uses.
2. **Injected logger struct as the fallback, not the default.** When a pure lib has diagnostics that *must* live next to the logic (complex internal state, tight loops, troubleshooting-only lines), it takes an optional `AstrOsLogger` struct of function pointers. Defaults to all-no-op. An ESP adapter in `lib/AstrOsUtility_ESP` provides `makeEspLogger()` wiring the pointers to `ESP_LOGx`.

## Why not just use ESP_LOG everywhere and stub it on native?

Tried that informally — the `esp_log.h` header drags in ESP-IDF registry headers that don't compile under `platform = native`. Stubbing them means maintaining a shim that has to track ESP-IDF version changes. The fn-ptr struct is ~15 lines, adds one indirect call per log site (no vtable, no heap), and decouples the pure lib from the ESP toolchain entirely.

## Scope

Foundation + one pilot extraction. ~8 tasks total, at the CLAUDE.md scope-guard boundary. If the task count creeps up during execution, split into two phases at the Part 1/Part 2 seam — Part 1 ships a foundation + `isPathSafe` proof point independently.

**Branching:** this plan should branch off `develop` *after* the Phase C PR merges, to avoid stacking unrelated commits. It does not touch any files Phase C modified except `CLAUDE.md` (Task 8), so it stays cleanly independent.

## Part 1 — Foundation

### Task 1: Create `lib/AstrOsLogging` (PURE)

- **New files:**
  - `lib/AstrOsLogging/include/AstrOsLogger.hpp` — declares `struct AstrOsLogger` (function pointers for `trace`, `debug`, `info`, `warn`, `error`; each takes `const char* tag, const char* fmt, ...` and forwards via `vprintf`-style). Default construction zeroes the pointers; an inline helper `astros_log_warn(logger, tag, "fmt", args...)` no-ops when the pointer is null, invokes it otherwise.
  - `lib/AstrOsLogging/README` — hard rule: "no ESP-IDF or FreeRTOS includes. Compiled under `[env:test]`."
- **Why not a class:** Avoids vtable, allows aggregate-initialization at compile time, and makes the "default = silent" property trivial.
- **Not yet used anywhere** — this task is purely introducing the type.

### Task 2: Create ESP adapter in `lib/AstrOsUtility_ESP`

- **New file:** `lib/AstrOsUtility_ESP/include/AstrOsLoggerEsp.h`
- Exposes `AstrOsLogger makeEspLogger();` whose function pointers wrap `esp_log_writev()` at the corresponding level.
- **New file:** `lib/AstrOsUtility_ESP/src/AstrOsLoggerEsp.cpp` — the tiny adapter itself.
- Coexists with the existing `logError()` helper in `AstrOsUtility_ESP.h`; no need to touch that.

### Task 3: Extend CI purity guard

- **Modify:** `.github/workflows/pr-validation.yml` — the existing "AstrOsMessaging native-purity guard" job greps the lib for forbidden includes. Parameterise that check over a list of pure libs (`AstrOsMessaging`, `AstrOsUtility`, `AstrOsLogging`). Future extractions just append to the list.
- Forbidden patterns: `#include <freertos/`, `#include <esp_`, `#include <driver/`, `#include <nvs_`, `#include <sdmmc_`, `#include <esp_vfs`, `#include <esp_now`, `#include <esp_wifi`.
- Fails the job with the exact offending line + path.

### Task 4: Move `isPathSafe` into `lib/AstrOsUtility` (proof point)

- **Why first**: exercises the whole foundation pipeline (pure lib + purity guard + native test) against already-written, already-QA'd logic. Small, risk-free, and validates the flow before the bigger pilot.
- **Modify:** `lib/AstrOsUtility/include/AstrOsPathUtils.hpp` (new) — `namespace AstrOsPathUtils { bool isPathSafe(const std::string&, std::string& reasonOut); }`. Returns the reject reason via out-param (or tuple) rather than an `ESP_LOGW` call — that's the "rich return" pattern.
- **Modify:** `lib/AstrOsStorageManager/src/AstOsStorageManager.cpp` — delete the internal `isPathSafe`; call `AstrOsPathUtils::isPathSafe` and log `reasonOut` at `ESP_LOGW` at the boundary (same behaviour as today, just split). Update the header to drop the private declaration.
- **New test:** `test/test_native/astros_path_utils_tests.cpp` — cases for each of the 5 reject conditions already enumerated in `.docs/qa/code-review-phase-c.md` §5 (empty, absolute, `..`, `//`, overlong).
- Adds a test file to the `[env:test]` target.

## Part 2 — Pilot: `AstrOsSerialMsgHandler` extraction

### Task 5: Create `lib/AstrOsSerialProtocol` (PURE)

- **New files:**
  - `lib/AstrOsSerialProtocol/include/AstrOsSerialProtocol.hpp`
  - `lib/AstrOsSerialProtocol/src/AstrOsSerialProtocol.cpp`
  - `lib/AstrOsSerialProtocol/README` (purity rule, same as AstrOsMessaging)
- **Shape of the API:**
  ```cpp
  struct DecodedCommand {
      AstrOsInterfaceResponseType responseType;
      std::string msgId;
      std::string peerMac;   // empty = broadcast
      std::string peerName;
      std::string message;
  };

  struct DecodeReject {
      std::string entry;            // the raw controller sub-string that failed
      DecodeRejectReason reason;    // WRONG_PART_COUNT | EMPTY_DEST | EMPTY_VALUE | UNKNOWN_TYPE
  };

  struct DecodeResult {
      std::vector<DecodedCommand> commands;
      std::vector<DecodeReject> rejects;
  };

  DecodeResult decodeSerialMessage(
      AstrOsSerialMessageType type,
      const std::string& msgId,
      const std::string& message,
      bool isMaster);

  AstrOsInterfaceResponseType mapResponseType(
      AstrOsSerialMessageType type, bool isMaster);
  ```
- Ports the logic from the current `handleRegistrationSync`, `handleDeployConfig`, `handleDeployScript`, `handleBasicCommand`, and `getResponseType` in `lib/AstrOsSerialMsgHandler/src/AstrOsSerialMsgHandler.cpp:70-231`.
- **Depends only on:** `lib/AstrOsMessaging` (for `AstrOsSerialMessageType`, separators) and `lib/AstrOsUtility` (for `AstrOsStringUtils::splitString`). Both already pure. No logger needed — the `rejects` vector is how the caller learns about skipped entries.

### Task 6: Rewrite `AstrOsSerialMsgHandler` as a thin adapter

- **Modify:** `lib/AstrOsSerialMsgHandler/src/AstrOsSerialMsgHandler.cpp`
  - `handleMessage` validates the wire format (existing `msgService.validateSerialMsg`), then calls `AstrOsSerialProtocol::decodeSerialMessage`, then:
    - Iterates `result.commands` and calls `sendToInterfaceQueue(cmd.responseType, cmd.msgId, cmd.peerMac, cmd.peerName, cmd.message)`.
    - Iterates `result.rejects` and emits `ESP_LOGW(TAG, "Invalid %s: %s", reason, entry)` at the boundary.
  - Delete `handleRegistrationSync`, `handleDeployConfig`, `handleDeployScript`, `handleBasicCommand`, `getResponseType` from both header and cpp.
  - `sendToInterfaceQueue`, `sendRegistraionAck`, `sendPollAckNak`, `sendBasicAckNakResponse` stay — they own queue handles and malloc/memcpy/xQueueSend, which is the MIXED layer's actual responsibility.
- **Modify:** `lib/AstrOsSerialMsgHandler/include/AstrOsSerialMsgHandler.hpp` — remove the five private member declarations.

### Task 7: Native tests for `AstrOsSerialProtocol`

- **New file:** `test/test_native/astros_serial_protocol_tests.cpp`
- Test cases:
  - `REGISTRATION_SYNC` produces one broadcast command with `REGISTRATION_SYNC` response type.
  - `DEPLOY_CONFIG` master path: `00:00:00:00:00:00` → `SET_CONFIG`; padawan path: specific MAC → `SEND_CONFIG`.
  - `DEPLOY_SCRIPT` reconstructs the `msgParts[2] + UNIT_SEPARATOR + msgParts[3]` payload correctly.
  - `RUN_SCRIPT`, `PANIC_STOP`, `FORMAT_SD`, `RUN_COMMAND`, `SERVO_TEST` each map master↔padawan response types correctly via `mapResponseType`.
  - Rejects: wrong part count (DEPLOY_CONFIG with 2 parts instead of 3), empty dest, empty value — each produces an entry in `rejects`, none in `commands`.
  - Mixed valid + invalid controllers in the same message — valid ones land in `commands`, invalid in `rejects`.

### Task 8: Docs + convention

- **Modify:** `CLAUDE.md` — in the "Library layout" section, add a classification column (PURE / MIXED / HARDWARE-ONLY) to the lib descriptions, and a short "Adding a new extracted lib" subsection listing the three steps: create lib directory with `README` stating the purity rule, add lib name to the CI purity guard list, prefer rich return values over logger injection.
- No change to `.docs/code-review/*` — this is pattern work, not review remediation.

## Files touched

**Created:**
- `lib/AstrOsLogging/include/AstrOsLogger.hpp`
- `lib/AstrOsLogging/README`
- `lib/AstrOsUtility_ESP/include/AstrOsLoggerEsp.h`
- `lib/AstrOsUtility_ESP/src/AstrOsLoggerEsp.cpp`
- `lib/AstrOsUtility/include/AstrOsPathUtils.hpp`
- `lib/AstrOsUtility/src/AstrOsPathUtils.cpp` (if impl needed; may be header-only)
- `lib/AstrOsSerialProtocol/include/AstrOsSerialProtocol.hpp`
- `lib/AstrOsSerialProtocol/src/AstrOsSerialProtocol.cpp`
- `lib/AstrOsSerialProtocol/README`
- `test/test_native/astros_path_utils_tests.cpp`
- `test/test_native/astros_serial_protocol_tests.cpp`

**Modified:**
- `.github/workflows/pr-validation.yml` (purity guard extended over lib list)
- `lib/AstrOsStorageManager/include/AstrOsStorageManager.hpp` (drop private `isPathSafe`)
- `lib/AstrOsStorageManager/src/AstOsStorageManager.cpp` (delegate to `AstrOsPathUtils`)
- `lib/AstrOsSerialMsgHandler/include/AstrOsSerialMsgHandler.hpp` (drop 5 private members)
- `lib/AstrOsSerialMsgHandler/src/AstrOsSerialMsgHandler.cpp` (rewrite `handleMessage` as adapter)
- `CLAUDE.md` (classification column + "Adding a new extracted lib" blurb)

## Existing code to reuse

- `lib/AstrOsUtility/include/AstrOsStringUtils.hpp::splitString` — pure, already used by the current handler.
- `lib/AstrOsMessaging` — `AstrOsSerialMessageType`, separator constants, `validateSerialMsg`.
- `lib/AstrOsUtility_ESP/include/AstrOsUtility_ESP.h::logError` — existing ESP-side helper; precedent for where the new `AstrOsLoggerEsp` adapter fits.
- `.docs/qa/code-review-phase-c.md` §5 — authoritative list of `isPathSafe` reject cases; drives the native-test coverage.

## Verification

- **Native tests:** `pio test -e test` — all existing tests still pass; `astros_path_utils_tests` and `astros_serial_protocol_tests` pass. Expected: 2 new test files contributing ~15-20 test cases.
- **Firmware builds:** `pio run -e lolin_d32_pro` and `pio run -e metro_s3` — clean build, no new warnings.
- **CI purity guard:** Push a throwaway commit that adds `#include <esp_log.h>` to `lib/AstrOsLogging/include/AstrOsLogger.hpp`, confirm the purity-guard job fails with a pointer to that line. Revert.
- **Runtime smoke (optional hardware):** Flash `metro_s3`, deploy a config and a script via the web UI, confirm the handler still dispatches correctly to interface queue. Inject a malformed controller record (e.g., a `DEPLOY_CONFIG` entry with only 2 `UNIT_SEPARATOR` parts) and confirm the `ESP_LOGW` at the boundary still fires with the reject reason.

## Task checklist

- [x] Task 1: Create `lib/AstrOsLogging` pure lib + README
- [x] Task 2: Create `lib/AstrOsUtility_ESP` logger adapter
- [x] Task 3: Extend CI purity guard over a lib list
- [x] Task 4: Extract `isPathSafe` into `lib/AstrOsUtility` + native test
- [x] Task 5: Create `lib/AstrOsSerialProtocol` pure lib
- [x] Task 6: Rewrite `AstrOsSerialMsgHandler` as thin adapter
- [x] Task 7: Add native tests for `AstrOsSerialProtocol`
- [x] Task 8: Update CLAUDE.md with classification + "Adding a new extracted lib" blurb

## What's explicitly not in this plan

- `AnimationController` engine extraction — separate follow-up plan once the pilot lands.
- `AstrOsEspNow` peer-registration FSM + fragmentation tests — separate follow-up plan.
- `Modules/*` extraction (Maestro protocol negotiation, serial command parsing) — separate follow-up plan.
- Any new logging injection inside `AstrOsSerialProtocol` — deliberately none, because the rich-return pattern (`DecodeResult` with `rejects`) makes it unnecessary. The injected-logger path is still available for later extractions where it's warranted.
