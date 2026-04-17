# AstrOsEspNow Extraction — Phase 1: Packet Handlers + Dispatch

## Context

`lib/AstrOsEspNow/src/AstrOsEspNowService.cpp` (1395 LOC) is the last large MIXED library that tangles ESP-IDF/FreeRTOS orchestration with pure message-handling logic. OTA firmware update support is landing next and will ride on ESP-NOW — it will add new packet types (`OTA_INIT`, `OTA_CHUNK`, `OTA_COMMIT`, `OTA_ACK/NAK`) that each need a payload parser + queue handoff exactly like the existing nine `handleXxx` methods. Today those handlers can only be exercised with a hardware integration harness, which is a poor fit for firmware-distribution code where bit-exact parsing and fragment reassembly matter.

Phase 1 extracts the nine pure packet-type handlers (the ones that only parse a payload and emit an interface-queue message) plus their dispatch switch into a new pure lib `lib_native/AstrOsEspNowProtocol`, mirroring the `lib_native/AstrOsSerialProtocol` pilot. Peer-state-entangled paths (registration + poll FSMs, the P0-9 `peers` vector concurrency fix) are **deferred to Phase 2** — including them would blow the 8-task scope guard and couple an OTA-readiness win to a separate concurrency refactor.

Intended outcome after Phase 1:

- Every OTA handler added in the upcoming OTA feature can be written and unit-tested under `pio test -e test` from the first commit.
- Nine existing handlers gain native test coverage (~30 cases across valid / short / multi-packet-pending / master-vs-padawan).
- Thin adapter in `AstrOsEspNow` shrinks by ~200 LOC.

## Scope decisions (locked in)

| Question | Decision |
|---|---|
| Lib name | `AstrOsEspNowProtocol` (parallel to `AstrOsSerialProtocol`) |
| API shape | Free functions in `namespace AstrOsEspNowProtocol`, matching `AstrOsSerialProtocol` |
| `PacketTracker` threading | Passed by reference into `handlePacket`. Adapter owns lifetime. |
| `AstrOsInterfaceResponseType` | **No move.** Include directly from `lib/AstrOsQueueMessages/include/AstrOsInterfaceResponseMsg.hpp` — it's already a pure header (`#include <string>` only) and `AstrOsSerialProtocol` already consumes it this way. |
| `isMasterNode` gating | Passed as `bool` parameter into `handlePacket`. Dispatcher enforces wrong-role rejection in the pure layer (testable). |
| Scope exclusions | `handleRegistrationReq/Registration/RegistrationAck`, `handlePoll/PollAck`, `pollPadawans`, `pollRepsonseTimeExpired` stay in adapter untouched. Dispatcher returns `UnsupportedType` for their packet types so the adapter's residual switch handles them. |
| Handlers included | 9 total: `Config`, `ConfigAckNak`, `ScriptDeploy`, `ScriptRun`, `CommandRun`, `PanicStop`, `FormatSD`, `ServoTest`, `BasicAckNak` |

## Public API

```cpp
// lib_native/AstrOsEspNowProtocol/include/AstrOsEspNowProtocol.hpp
#include <AstrOsInterfaceResponseMsg.hpp>   // enum + struct (already pure header)
#include <AstrOsMessaging.hpp>              // astros_packet_t, AstrOsPacketType
#include <PacketTracker.hpp>
#include <optional>
#include <string>

namespace AstrOsEspNowProtocol {

struct InterfaceMessage {
    AstrOsInterfaceResponseType responseType = AstrOsInterfaceResponseType::UNKNOWN;
    std::string msgId;
    std::string peerMac;
    std::string peerName;
    std::string message;
};

enum class HandlerStatus {
    Ok,                 // handler produced an InterfaceMessage
    Pending,            // multi-packet message awaiting more fragments (no-op, try later)
    InvalidPayload,     // malformed (wrong part count, empty dest, etc.)
    WrongRole,          // packet addressed to a role this node doesn't hold
    UnsupportedType,    // Phase 2 handlers — adapter falls through to its legacy switch
    UnknownType,        // packet.packetType == UNKNOWN or out of range
};

struct HandlerResult {
    HandlerStatus status;
    std::optional<InterfaceMessage> message;
    std::string diagnostic;  // empty on Ok/Pending; populated on error paths for caller to log
};

HandlerResult handlePacket(const astros_packet_t& packet,
                           PacketTracker& tracker,
                           bool isMasterNode);

// Exposed for direct unit testing
AstrOsInterfaceResponseType mapResponseType(AstrOsPacketType packetType);

}  // namespace AstrOsEspNowProtocol
```

## Task checklist (6 tasks, one commit each)

- [ ] **Task 1 — Scaffold pure lib.** Create `lib_native/AstrOsEspNowProtocol/{include,src}` with header from the API block above, empty `.cpp`, and a `README` stating the purity rule + forbidden-include list (copy from `lib_native/AstrOsAnimationEngine/README`). Append `lib_native/AstrOsEspNowProtocol` to `PURE_LIBS` in `.github/workflows/pr-validation.yml`. *Verify:* `pio run -e test` builds with a no-op `.cpp`; purity-grep step passes.
- [ ] **Task 2 — Implement `mapResponseType` + `extractPayload` helper.** Port the switch from `AstrOsEspNow::getInterfaceResponseType` (`lib/AstrOsEspNow/src/AstrOsEspNowService.cpp:1133-1156`). Implement an internal `extractPayload(packet, tracker)` that replicates `handleMultiPacketMessage` (`:1161-1179`) using the passed-in tracker. *Verify:* GoogleTest covers all enum mappings + single-packet extract + multi-packet first-fragment returns `Pending` + multi-packet completion returns assembled payload.
- [ ] **Task 3 — Implement 8 single-record handlers.** Port `handleConfig` (`:768`), `handleConfigAckNak` (`:822`), `handleScriptDeploy` (`:847`), `handleScriptRun` (`:885`), `handleCommandRun` (`:951`), `handlePanicStop` (`:916`), `handleFormatSD` (`:986`), `handleServoTest` (`:1017`). Each is ~20 lines: `extractPayload` → `splitString(UNIT_SEPARATOR)` → part-count guard → `InterfaceMessage`. *Verify:* for each handler, tests cover (valid → `Ok`; short payload → `InvalidPayload` + diagnostic; multi-packet pending → `Pending`).
- [ ] **Task 4 — Implement `handleBasicAckNak` + top-level `handlePacket` dispatcher.** `handleBasicAckNak` ports from `:1098-1128`. Dispatcher mirrors the switch in `handleMessage` (`:323-393`) but returns `UnsupportedType` for `REGISTRATION_*`, `POLL_*`, and any other peer-state types. Role check (`isMasterNode`) returns `WrongRole` where appropriate. *Verify:* one test per supported packet type routes to the right handler; unsupported types return `UnsupportedType`; wrong-role packets return `WrongRole`.
- [ ] **Task 5 — Adapter rewrite.** In `lib/AstrOsEspNow/src/AstrOsEspNowService.cpp`, rewrite `handleMessage` so the pre-switch path calls `AstrOsEspNowProtocol::handlePacket(packet, this->packetTracker, this->isMasterNode)` and, on `Ok`, forwards the returned `InterfaceMessage` through the existing `sendToInterfaceQueue(responseType, msgId, peerMac, peerName, message)` wiring. On `InvalidPayload`/`WrongRole`/`UnknownType`, log `result.diagnostic` via `ESP_LOGE(TAG, ...)`. On `Pending`, return (no-op). On `UnsupportedType`, fall through to the residual switch for Registration/Poll handlers. Delete the nine now-orphaned private methods and `getInterfaceResponseType`. If `handleMultiPacketMessage` has no callers left, delete it too (confirm). *Verify:* `pio run -e lolin_d32_pro` and `pio run -e metro_s3` build clean, no new warnings.
- [ ] **Task 6 — Hardware smoke + docs update.** Flash one master + one padawan (metro_s3 is fine for both). Exercise: deploy config → deploy script → run script → panic stop → servo test → format-SD round-trip. Confirm interface-queue behaviour is identical to pre-extraction (same log lines, same display state changes). Update `CLAUDE.md` Library-layout table: add `lib_native/AstrOsEspNowProtocol` row with classification `PURE` and one-line summary. *Verify:* QA plan at `.docs/qa/espnow-handlers-extraction.md` committed alongside; all six ESP-NOW exercises pass; `pio test -e test` still green.

## Critical files

**Created:**

- `lib_native/AstrOsEspNowProtocol/include/AstrOsEspNowProtocol.hpp` — public API above
- `lib_native/AstrOsEspNowProtocol/src/AstrOsEspNowProtocol.cpp` — nine handlers + dispatcher + helpers
- `lib_native/AstrOsEspNowProtocol/README` — purity rule, forbidden-include list, rich-return convention
- `test/test_native/astros_espnow_protocol_tests.cpp` — GoogleTest suite, flat namespace, ~30 cases
- `.docs/qa/espnow-handlers-extraction.md` — manual QA plan for Task 6

**Modified:**

- `lib/AstrOsEspNow/src/AstrOsEspNowService.cpp` — rewrite `handleMessage` as adapter; delete 9 private handler methods + `getInterfaceResponseType` (+ maybe `handleMultiPacketMessage`)
- `lib/AstrOsEspNow/src/AstrOsEspNowService.hpp` — drop the 9 private declarations
- `.github/workflows/pr-validation.yml` — append `lib_native/AstrOsEspNowProtocol` to `PURE_LIBS` array
- `CLAUDE.md` — add new lib row to Library-layout table

## Existing code to reuse

| Utility | Location | Role |
|---|---|---|
| `AstrOsStringUtils::splitString` | `lib_native/AstrOsUtility/include/AstrOsStringUtils.hpp` | Every handler parses `UNIT_SEPARATOR`-delimited payloads |
| `UNIT_SEPARATOR`, `RECORD_SEPARATOR` | `lib_native/AstrOsUtility/include/AstrOsConstants.h` | Payload separators |
| `astros_packet_t`, `AstrOsPacketType` | `lib_native/AstrOsMessaging/src/AstrOsEspNowMessageService.hpp` | Input type to every handler |
| `PacketTracker::addPacket`, `getMessage` | `lib_native/AstrOsMessaging/src/PacketTracker.hpp` | Multi-packet reassembly, already pure |
| `AstrOsInterfaceResponseType`, `astros_interface_response_t` | `lib/AstrOsQueueMessages/include/AstrOsInterfaceResponseMsg.hpp` | Output enum — pure-safe header, included directly |
| `packet_tracker_tests.cpp` fragment fixtures | `test/test_native/packet_tracker_tests.cpp` | Helpers for constructing multi-packet sequences in Task 3 |
| `handleMultiPacketMessage` body | `lib/AstrOsEspNow/src/AstrOsEspNowService.cpp:1161-1179` | Template for the pure `extractPayload` helper |
| `getInterfaceResponseType` switch | `lib/AstrOsEspNow/src/AstrOsEspNowService.cpp:1133-1156` | Port verbatim to `mapResponseType` |

## Verification

- **Native tests:** `pio test -e test` — all existing suites pass; new `astros_espnow_protocol_tests.cpp` contributes ~30 cases (9 handlers × 3 path outcomes + dispatch matrix + `mapResponseType` enumeration).
- **Firmware builds:** `pio run -e lolin_d32_pro` and `pio run -e metro_s3` compile clean, no new warnings.
- **CI purity guard:** PR validation `native-purity` job now scans `lib_native/AstrOsEspNowProtocol`.
- **Hardware smoke:** metro_s3 master + metro_s3 padawan. Drive the interface app through the six ESP-NOW flows above; compare against the pre-extraction behaviour baseline (same display state, same log lines, same interface-queue responses).
- **Regression shield:** Registration/Poll/PollAck paths are deliberately untouched — if they break, root cause is elsewhere.

## What's explicitly out of scope (Phase 2 candidates)

- Pure `PeerList` class replacing the unprotected `peers` vector (fixes code-review P0-9).
- Registration FSM extraction (`handleRegistrationReq/Registration/RegistrationAck` → pure state-transition functions).
- Poll cycle FSM extraction (`pollPadawans`, `handlePollAck`, `pollRepsonseTimeExpired`) with an injectable time source.
- Spin-wait getter cleanup (code-review P1-16 — `getMac`, `getName`, `getFingerprint`).
- OTA-specific packet types and handlers — those land as *consumers* of this Phase 1 pattern, in their own plan.
