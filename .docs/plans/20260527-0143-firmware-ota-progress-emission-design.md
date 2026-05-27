# FW_PROGRESS Emission + M4 Flash-Failure Placeholder — Design

## Problem

When an M4 deploy completes the byte transfer + on-device verification, the
firmware sends `FW_DEPLOY_DONE OK` to the server. The server's per-controller
state machine
(`astros_api/src/firmware/flash_job_state_machine.ts:18-32`) requires the
chain `Sending → Verifying → Rebooting → VersionConfirmed`. M4 emits no
`FW_PROGRESS` frames, so the controller never advances past `Sending`. When
`OK` arrives, the orchestrator attempts `Sending → VersionConfirmed`, the
state-machine throws `illegal flash-job transition`, and the orchestrator
maps the throw to `protocol_violation` via `failDeployPhase`. The UI shows
**"Flash failed: illegal flash-job transition: SENDING → VERSION_CONFIRMED
(controllerId=…)"** on a deploy that demonstrably succeeded.

The server-side band-aid (loosen the state machine to allow
`Sending → VersionConfirmed`) was rejected — it lies about the lifecycle
and hides the M4-vs-PR-set-2 gap. The right answer is to honestly signal
M4's capabilities on the wire, and to make the missing-flash-step
**visible** in the UI so the operator workflow gets exercised end-to-end
before PR set 2 builds the real flash-commit step.

## Design intent

> *"Test like we operate. Implement minimal placeholders so we can properly
> test the expected behaviors of the workflow."*

Two principles drive every choice in this design:

1. **Honest stage signals.** The firmware emits `FW_PROGRESS` for every
   stage it actually traverses. No misleading transitions, no fake
   `REBOOTING` to satisfy the state machine.
2. **Deliberate placeholder failure.** Because M4 cannot commit the boot
   partition, the padawan **deliberately** reports the flash step as
   failed. This isn't a workaround — it's the explicit signal that PR set
   2's flash-commit work is still pending, and it lets the failure-handling
   UX get exercised on every M4 deploy.

## Target end-to-end behavior

After this design lands, a successful M4 deploy renders in the UI as:

| Stage row | State |
|---|---|
| download | ✓ done |
| transfer | ✓ done |
| verify | ✓ done |
| flash | ! failed |
| reboot | idle |

Failure result-bar: `⚠ <controllerName> failed during Flash` with the reason
`flash_not_implemented` surfaced in the controller detail. The operator sees
exactly the post-PR-set-2 failure shape, just with a known-pending root
cause.

Note: the UI's current `FIRMWARE_STAGES` constant
(`stageRowState.ts:3`) declares row order
`['download', 'transfer', 'flash', 'verify', 'reboot']` — verify *after*
flash. That order is wrong relative to the firmware's actual lifecycle
(transfer → verify → flash → reboot) and renders nonsensically in either
M4 or PR set 2: the stage row positions don't match the lifecycle
progression direction. This design **reorders** the constant to
`['download', 'transfer', 'verify', 'flash', 'reboot']` so row order
matches lifecycle order. This is a small drive-by correctness fix
inside the surface area we're touching.

## Architecture overview

```
PADAWAN                          MASTER (OtaForwarder)             SERVER
                                  │
                                  │ FW_TRANSFER_BEGIN/CHUNK_ACK ...   (existing flow)
                                  │
                                  │ ─── FW_PROGRESS stage=SENDING ─── ▶ Sending
                                  │     (running bytesSent/totalBytes throttled)
                                  │
   OTA_BEGIN  ◀──────────────────│
   OTA_DATA   ◀──────────────────│  (many)
   OTA_END    ◀──────────────────│
                                  │
   handleEnd → 3 integrity gates  │
   pass on padawan side           │
   (SHA, esp_ota_end, readback)   │
                                  │
   OTA_END_ACK with new           │
   status FLASH_NOT_IMPLEMENTED ─▶│
                                  │ ─── FW_PROGRESS stage=VERIFYING ─ ▶ Verifying
                                  │
                                  │ ─── FW_PROGRESS stage=FLASHING ── ▶ Flashing
                                  │     (synthesized — master enters flash state
                                  │      before mapping padawan's status into results_)
                                  │
                                  │ ─── FW_DEPLOY_DONE result        ─ ▶ Flashing → Failed
                                  │     outcome=FAILED                    (reason: flash_not_implemented)
                                  │     reason=flash_not_implemented
```

The padawan **does** verify successfully (the three integrity gates pass),
which is why the master can honestly emit `VERIFYING` after `OTA_END_ACK`
arrives. The padawan then explicitly reports that the next step — the
boot-partition flip — is not implemented, via the new `OtaEndStatus` value.

## Wire-format changes

### `OtaEndStatus` — new value `FLASH_NOT_IMPLEMENTED`

Defined in `lib_native/AstrOsMessaging/src/OtaWirePayloads.hpp`. The
existing enum:

```cpp
enum class OtaEndStatus : uint8_t {
    OK = 0,
    HASH_MISMATCH = 1,
    WRITE_ERROR = 2,
    // NEW:
    FLASH_NOT_IMPLEMENTED = 3,
};
```

Semantics: "Transfer + on-device verification succeeded; the flash-commit
step (boot-partition flip) is intentionally not implemented in this
firmware build." Distinct from `WRITE_ERROR` (which is a real failure of
`esp_ota_write`) and from `HASH_MISMATCH` (data corruption). This value is
**only valid when emitted by an M4 firmware build that has chosen this
placeholder strategy**; PR set 2 firmware will not emit it and may
eventually remove it.

A native test pin (`test/test_native/`) covers the round-trip of the new
enum value through `buildOtaEnd*` / `parseOtaEnd*`.

### `FW_PROGRESS` — implement the existing-but-unbuilt contract

The server's wire contract is already specified
(`message_handler.ts:301-332`):

```
FW_PROGRESS<header>transferId<US>controllerId<US>stage<US>bytesSent<US>totalBytes<US>detail
```

- `stage` is the numeric `FwStage` enum value (currently 0..6: Queued,
  UploadingToMaster, Sending, Verifying, Rebooting, VersionConfirmed,
  Failed; this design adds a new value — see below).
- `bytesSent` / `totalBytes` are decimal unsigned integers. For
  byte-meaningful stages (Sending) they carry real values; for boundary
  events (Verifying, Flashing) they carry `totalBytes` in both (i.e.,
  "100% of bytes are accounted for at this point") so the server's
  byte-throttle path stays consistent.
- `detail` is a free-form string. For M4's Flashing emission, this
  carries `pr_set_1_placeholder` so server logs can attribute the synthetic
  emission to its origin.

`getFwProgress(...)` is added to
`lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.{hpp,cpp}`
following the same pattern as `getFwDeployDone` / `getFwTransferEndAck`.
`sendFwProgress(...)` is a thin wrapper in
`lib/AstrOsSerialMsgHandler/src/AstrOsSerialMsgHandler.cpp` that calls the
builder and posts to the interface-response queue. Native round-trip test
added.

## Server-side changes (AstrOs.Server)

### New `FwStage.Flashing`

In `astros_api/src/models/firmware/firmware_messages.ts`:

```typescript
export enum FwStage {
  Queued = 0,
  UploadingToMaster = 1,
  Sending = 2,
  Verifying = 3,
  Flashing = 4,            // NEW
  Rebooting = 5,           // renumbered
  VersionConfirmed = 6,    // renumbered
  Failed = 7,              // renumbered
}
```

The numeric renumbering is safe because the enum values are not persisted
anywhere — they flow only between firmware and server within a single
deploy, and both sides will be updated together. The over-the-wire stage
field continues to use the numeric value.

### State-machine update

In `astros_api/src/firmware/flash_job_state_machine.ts:18-32`:

```typescript
[FwStage.Verifying, new Set([FwStage.Verifying, FwStage.Flashing, FwStage.Failed])],
[FwStage.Flashing,  new Set([FwStage.Flashing,  FwStage.Rebooting, FwStage.Failed])], // NEW
[FwStage.Rebooting, new Set([FwStage.Rebooting, FwStage.VersionConfirmed, FwStage.Failed])],
```

The `Sending → VersionConfirmed` transition stays **illegal** — the goal is
not to weaken the state machine. M4 will never emit OK directly from
Sending after this change; it always goes Sending → Verifying → Flashing
→ Failed.

### UI stage mapping update

In `astros_vue/src/utils/firmwareStageMapping.ts:15-47`:

```typescript
case 'FLASHING':
  return 'flash';
```

`controllerStatePillKind` also gets a `FLASHING → 'updating'` arm.

### `ServerFwStage` type

In `astros_vue/src/types/firmware.ts`, the union type adds `'FLASHING'`.

### `FIRMWARE_STAGES` row-order fix

In `astros_vue/src/components/firmware/firmwareStagesList/stageRowState.ts:3`,
reorder the constant so row order matches the firmware lifecycle:

```typescript
// BEFORE — verify rendered after flash, contradicts lifecycle order
export const FIRMWARE_STAGES = ['download', 'transfer', 'flash', 'verify', 'reboot'] as const;

// AFTER — matches transfer → verify → flash → reboot lifecycle
export const FIRMWARE_STAGES = ['download', 'transfer', 'verify', 'flash', 'reboot'] as const;
```

Tests in `stageRowState.spec.ts` that hard-code stage indices need to be
audited; tests that just iterate `FIRMWARE_STAGES` are unaffected.

### Tests

- State-machine tests: pin the new transitions (`Verifying → Flashing`,
  `Flashing → Rebooting`, `Flashing → Failed`, `Sending → Flashing` is
  still illegal, etc.).
- Orchestrator tests: simulate M4 sequence (FW_PROGRESS Sending →
  Verifying → Flashing → FW_DEPLOY_DONE FAILED) and assert the controller
  reaches `Failed` cleanly with the reason propagated.
- Frontend `stageRowState.spec.ts`: confirm `failedStage='flash'` produces
  the expected row layout (transfer/verify done, flash failed, reboot
  idle).
- I18n: existing `failed_summary: "⚠ {label} failed during {stage}"` works
  unchanged with stage='Flash'.

## Firmware emission cadence

In `lib/OtaForwarder/src/OtaForwarder.cpp` (master only — padawans never
touch FW_PROGRESS; that channel is master → server):

| Trigger | Stage | bytesSent | totalBytes | detail |
|---|---|---|---|---|
| `startNextPadawan` after `emitOtaBeginFrame` | SENDING | 0 | firmwareTotalSize_ | `""` |
| Every ~5% of `firmwareTotalSize_` during streaming | SENDING | running | firmwareTotalSize_ | `""` |
| `handleEndAck` with OK or FLASH_NOT_IMPLEMENTED | VERIFYING | firmwareTotalSize_ | firmwareTotalSize_ | `""` |
| Immediately before mapping the padawan's status into `results_` (in `completeCurrentPadawan` and the M4-specific flash-not-implemented path) | FLASHING | firmwareTotalSize_ | firmwareTotalSize_ | `"pr_set_1_placeholder"` |

The throttling at 5% intervals matches the M5 plan cadence in
`20260523-1023-firmware-ota-mesh-forward-design.md:391-405`. For a 1.2 MB
firmware image, that's ~20 SENDING events + 1 VERIFYING + 1 FLASHING per
padawan.

## Padawan-side change

In `lib/OtaWriter/src/OtaWriter.cpp::handleEnd`, when the three integrity
gates all pass (the current `OK` path):

```cpp
// All three gates passed. M4 ships transfer + verify but does NOT
// commit to the boot partition (no esp_ota_set_boot_partition).
// Explicitly signal this to the master so the deploy UX shows the
// flash step as a known-pending failure. Replaced with OtaEndStatus::OK
// when PR set 2 implements the boot-commit step.
sendEndAck(mac, xferId, OtaEndStatus::FLASH_NOT_IMPLEMENTED, sha256Computed);
```

The streamed digest is still echoed back (it's accurate; the bytes are
on the inactive partition exactly as expected). The change is purely the
status byte.

A code comment block at the call site explicitly marks this as a
placeholder and references this design doc.

## OtaForwarder result mapping

In `lib/OtaForwarder/src/OtaForwarder.cpp::handleEndAck` (the master-side
consumer of the padawan's `OTA_END_ACK`):

| Padawan status | OtaForwarder outcome | FW_DEPLOY_DONE result |
|---|---|---|
| OK | (unreachable in M4) | OK |
| HASH_MISMATCH | FAILED | `controllerId, FAILED, "", "hash_mismatch"` |
| WRITE_ERROR | FAILED | `controllerId, FAILED, "", "write_error"` |
| **FLASH_NOT_IMPLEMENTED** | **FAILED** | `controllerId, FAILED, "", "flash_not_implemented"` |

The reason string flows through `FwDeployDoneResult.error` to the UI's
controller-detail view.

## Non-goals

- **Not adding `Rebooting` or `VersionConfirmed` emission in M4.** Those
  are PR set 2's responsibility. After this design, M4 will never reach
  those stages — every M4 deploy terminates at `Failed` via the
  Flashing step. That's the point.
- **Not changing the master self-flash path.** The current `master_self_flash_pending`
  marker (Phase C work) stays as-is.
- **Not adding a "deferred to PR set 2" outcome value to FW_DEPLOY_DONE.**
  Existing OK/FAILED is sufficient; the reason string carries the
  M4-specific detail.
- **Not addressing the agent's flagged `collectFailedControllers` bug.**
  That bug exists but only affects the *stage label* shown in the
  failure summary when controllers reach Failed via different routes —
  in M4 every controller fails the same way (via Flashing), so the global
  `currentStage` snapshot will accurately equal each per-controller stage.
  Fix can land separately.

## Migration plan

The new enum value (`OtaEndStatus::FLASH_NOT_IMPLEMENTED`) and new server
stage (`FwStage.Flashing`) need to land together — a server running pre-
this-design code receiving the new status would log an unknown-outcome
warning, and an M4 firmware running pre-this-design server code would
hit the same illegal-transition bug. Coordination:

1. Land server-side enum + state-machine + UI mapping changes first.
   Pre-existing M4 firmware deploys continue to hit the
   illegal-transition bug (no regression).
2. Land firmware FW_PROGRESS emission + padawan placeholder change. M4
   deploys against the updated server now show the intended UI flow.
3. When PR set 2 ships:
   - Padawan sends `OK` again (real flash commit succeeded).
   - Firmware adds `REBOOTING` + `VERSION_CONFIRMED` FW_PROGRESS emission.
   - Server's `Flashing → Rebooting → VersionConfirmed` path becomes the
     happy path.
   - `OtaEndStatus::FLASH_NOT_IMPLEMENTED` can be retired (or kept as a
     debug/test-mode hook).

## Phasing

Single phase. ~6 tasks, two-repo coordination. Below the "more than ~8
discrete tasks" scope-guard threshold from `CLAUDE.md`. Tasks:

1. **AstrOs.ESP — wire format.** Add `OtaEndStatus::FLASH_NOT_IMPLEMENTED`,
   native round-trip test, update `OtaWirePayloads.hpp` comment.
2. **AstrOs.ESP — `getFwProgress` + `sendFwProgress`.** Builder + wrapper +
   native round-trip test.
3. **AstrOs.ESP — `OtaForwarder` emission.** Wire SENDING/VERIFYING/FLASHING
   per the cadence table.
4. **AstrOs.ESP — `OtaWriter::handleEnd` placeholder.** Send
   `FLASH_NOT_IMPLEMENTED` instead of OK; comment block referencing this
   design.
5. **AstrOs.Server — enum + state machine + mapping + row order.** Add
   `FwStage.Flashing`, update transitions, update UI mapping, update
   `ServerFwStage` type, reorder `FIRMWARE_STAGES` to
   `['download', 'transfer', 'verify', 'flash', 'reboot']`, update tests.
6. **AstrOs.Server — orchestrator integration test.** Simulate M4 sequence
   end-to-end (FW_PROGRESS chain + FW_DEPLOY_DONE FAILED with
   flash_not_implemented), assert UI-visible state.

Bench validation: a 2-padawan deploy from the server UI must render
`✓ transfer ! flash` and surface the `flash_not_implemented` reason in
the controller detail view.

## Related references

- M3 master-side design: `.docs/plans/20260523-1023-firmware-ota-mesh-forward-design.md`
- Original FW_PROGRESS cadence proposal: same file, lines 391-405
- Cross-repo decomposition: `AstrOs.Server/.docs/completed_plans/2026/04/27/20260427-2202-firmware-ota-decomposition.md`
- Server state machine: `astros_api/src/firmware/flash_job_state_machine.ts`
- Server FW_PROGRESS parser: `astros_api/src/serial/message_handler.ts:301-332`
- UI stage list: `astros_vue/src/components/firmware/firmwareStagesList/stageRowState.ts:3`
- UI stage mapping: `astros_vue/src/utils/firmwareStageMapping.ts`
