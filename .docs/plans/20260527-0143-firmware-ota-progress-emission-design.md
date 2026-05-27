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
                                  │ ─── FW_PROGRESS stage=VERIFYING ─ ▶ Verifying
                                  │     (emitted at OTA_END send so the
                                  │      verify row lights up while the
                                  │      padawan is doing the 3 gates)
   handleEnd → 3 integrity gates  │
   pass on padawan side           │
   (SHA, esp_ota_end, readback)   │
                                  │
   OTA_END_ACK OK ───────────────▶│
   (verification succeeded;       │
    no flash committed yet)       │
                                  │ ─── FW_PROGRESS stage=FLASHING ── ▶ Flashing
                                  │     (flash row lights up while padawan
                                  │      is in its pre-flash delay)
   Padawan delays 2 s             │
   (visible window for the        │
    Flashing row)                 │
                                  │
   ┌──────────────────────────────┴──────────────────────────────┐
   │ M4 (this PR):                                               │
   │   Padawan sends OTA_FLASH_RESULT with                       │
   │     status=FLASH_NOT_IMPLEMENTED                            │
   │                                                             │
   │ PR set 2 (future):                                          │
   │   Padawan calls esp_ota_set_boot_partition                  │
   │   Padawan sends OTA_FLASH_RESULT status=OK (or FAILED       │
   │     if the boot-partition flip returned error)              │
   │   Padawan triggers reboot                                   │
   └──────────────────────────────┬──────────────────────────────┘
                                  │
   OTA_FLASH_RESULT ─────────────▶│
                                  │
                                  │ M4: Flashing → Failed
                                  │ ─── FW_DEPLOY_DONE outcome=FAILED ▶
                                  │     reason=flash_not_implemented
                                  │
                                  │ PR set 2: Flashing → Rebooting
                                  │ ─── FW_PROGRESS stage=REBOOTING ─ ▶
                                  │     (continues to VERSION_CONFIRMED
                                  │      via post-reboot heartbeat path)
```

The padawan **does** verify successfully (the three integrity gates pass),
sends `OTA_END_ACK OK`, and then waits 2 s before reporting the flash
outcome via a separate `OTA_FLASH_RESULT` message. The 2 s wait gives the
server-visible `Flashing` row a chance to render before the terminal
transition lands — without it, `FLASHING` and `FAILED` would arrive back-
to-back at the orchestrator and the row would never render as `'current'`.

This split also makes the wire protocol honest about the two lifecycle
phases: `OTA_END_ACK` reports verification done, `OTA_FLASH_RESULT`
reports the flash-commit outcome. In M4 the placeholder behavior is
expressed entirely on the new message; the existing `OTA_END_ACK` enum
is untouched.

## Wire-format changes

### New message: `OTA_FLASH_RESULT`

Padawan → master, sent after the padawan's 2 s pre-flash delay. Reports
the outcome of the flash-commit step distinct from the verification step
that `OTA_END_ACK` already covers.

Defined alongside the existing OTA payloads in
`lib_native/AstrOsMessaging/src/OtaWirePayloads.hpp`:

```cpp
enum class OtaFlashStatus : uint8_t {
    OK = 0,                     // esp_ota_set_boot_partition succeeded (PR set 2)
    FLASH_NOT_IMPLEMENTED = 1,  // M4 placeholder — flash step deliberately skipped
    FAILED = 2,                 // esp_ota_set_boot_partition returned error (PR set 2)
};

struct __attribute__((packed)) OtaFlashResultPayload {
    uint8_t  xferId;            // matches the active transfer
    uint8_t  status;            // OtaFlashStatus
    uint8_t  reasonLen;         // length of reason string, 0..63
    char     reason[63];        // optional reason / detail string; truncated, not NUL-required
};
```

`reason` is populated on `FLASH_NOT_IMPLEMENTED` (typically the literal
`pr_set_1_placeholder`) and on `FAILED` (the underlying `esp_err_t` name).
On `OK` it is the empty string.

Wire-format builder + parser pair added to
`lib_native/AstrOsMessaging/src/OtaWirePayloads.{hpp,cpp}`:
`buildOtaFlashResult(...)` and `parseOtaFlashResult(...)`. Native
round-trip tests in `test/test_native/astros_ota_espnow_messages_tests.cpp`
cover OK, FLASH_NOT_IMPLEMENTED-with-reason, FAILED-with-reason, and
malformed/truncated rejection.

Packet type enum gets a new variant in
`lib_native/AstrOsEspNowProtocol/include/AstrOsEspNowProtocol.hpp`:

```cpp
enum class AstrOsPacketType : uint8_t {
    // ... existing ...
    OTA_FLASH_RESULT = N,  // padawan -> master flash outcome
};
```

Dispatch added to `AstrOsEspNow::routeOtaToForwarder` (master-side
inbound) so the master forwarder receives the new message in its queue.

### `FW_PROGRESS` — implement the existing-but-unbuilt contract

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
| `emitOtaEndFrame` — immediately after the OTA_END frame is enqueued for the padawan | VERIFYING | firmwareTotalSize_ | firmwareTotalSize_ | `""` |
| `handleEndAck` on OK arrival from padawan | FLASHING | firmwareTotalSize_ | firmwareTotalSize_ | `""` |

The throttling at 5% intervals matches the M5 plan cadence in
`20260523-1023-firmware-ota-mesh-forward-design.md:391-405`. For a 1.2 MB
firmware image, that's ~20 SENDING events + 1 VERIFYING + 1 FLASHING per
padawan.

**Critical trigger placement**:
- `VERIFYING` is emitted when the master *sends* `OTA_END` (not when
  `OTA_END_ACK` arrives back). This makes the verify row light up while
  the padawan is doing the 3 integrity gates, instead of flashing on
  for milliseconds at the transition to flash.
- `FLASHING` is emitted when `OTA_END_ACK OK` arrives from the padawan.
  The padawan then sits in its 2 s pre-flash delay; the flash row is
  visible for the full duration of that delay. When `OTA_FLASH_RESULT`
  eventually arrives, the master maps it into `FW_DEPLOY_DONE` per the
  table below.

No FW_PROGRESS is emitted on `OTA_FLASH_RESULT` arrival — the message's
status field drives the per-padawan result in `FW_DEPLOY_DONE`, and the
server state machine takes the controller from `Flashing → Failed` (M4)
or `Flashing → Rebooting → VersionConfirmed` (PR set 2, with REBOOTING /
VERSION_CONFIRMED emitted via separate triggers not in scope for this
PR).

## Padawan-side change

In `lib/OtaWriter/src/OtaWriter.cpp::handleEnd`, when the three integrity
gates all pass:

1. Send `OTA_END_ACK OK` immediately with the streamed digest (current
   behavior, unchanged).
2. Schedule a 2 s delay before the flash-result emission. Implementation
   options:
   - **vTaskDelay** (`vTaskDelay(pdMS_TO_TICKS(2000))`) — blocks
     `otaWriterTask` for 2 s. Simple. The task has no pipelined work to
     do during this window (OTA is single-shot per transfer; no other
     messages drain from this queue mid-transfer), so blocking is
     acceptable. Recommended for M4.
   - **esp_timer one-shot** — schedules a callback that posts
     `OTA_WR_FLASH_FIRE` into the writer queue, which then sends the
     OTA_FLASH_RESULT and (in PR set 2) calls
     `esp_ota_set_boot_partition`. More complex but non-blocking.
     Probably correct for PR set 2 since the post-flash reboot path is
     intricate; can be deferred from M4.
3. After the delay, send `OTA_FLASH_RESULT` with
   `status = OtaFlashStatus::FLASH_NOT_IMPLEMENTED`,
   `reason = "pr_set_1_placeholder"`. PR set 2 replaces this with a
   real `esp_ota_set_boot_partition` call followed by
   `OtaFlashResult(OK)` or `OtaFlashResult(FAILED)` per the IDF return.

A comment block at the placeholder call site explicitly marks this as
M4 placeholder behavior and references this design doc by filename.

The padawan's existing `currentXferId_` / `currentMasterMac_` per-
transfer state is retained for the duration of the delay so the
follow-up flash-result emission addresses the right peer.

## OtaForwarder result mapping

Two padawan-originated messages now feed the master's per-padawan result:

**`OTA_END_ACK` (existing)** — verification outcome only. Master uses this
to either advance to `FLASHING` (on OK) or record an early failure (on
HASH_MISMATCH / WRITE_ERROR):

| End-ack status | Master action | FW_DEPLOY_DONE result |
|---|---|---|
| OK | emit FW_PROGRESS FLASHING; wait for OTA_FLASH_RESULT | (deferred — see flash-result table) |
| HASH_MISMATCH | record FAILED immediately; cancel further wait | `controllerId, FAILED, "", "hash_mismatch"` |
| WRITE_ERROR | record FAILED immediately; cancel further wait | `controllerId, FAILED, "", "write_error"` |

**`OTA_FLASH_RESULT` (new)** — flash-commit outcome:

| Flash-result status | OtaForwarder outcome | FW_DEPLOY_DONE result |
|---|---|---|
| OK | OK (PR set 2 only — never reached in M4) | `controllerId, OK, finalVersion, ""` |
| **FLASH_NOT_IMPLEMENTED** | **FAILED** | `controllerId, FAILED, "", "flash_not_implemented"` |
| FAILED | FAILED | `controllerId, FAILED, "", reason` (from the wire) |

The reason string flows through `FwDeployDoneResult.error` to the UI's
controller-detail view.

**Master-side flash-result wait timeout.** After emitting FLASHING the
master starts a timer with a generous timeout (proposed: 10 s, configurable
via a `kFlashResultTimeoutUs` constant in `OtaForwarder.hpp`). If
`OTA_FLASH_RESULT` does not arrive within the window, the master records
the padawan as FAILED with reason `"flash_result_timeout"`. This covers
the "padawan crashed during the pre-flash delay" failure mode that would
otherwise leave the master waiting forever.

**New forwarder phase: `AWAITING_FLASH_RESULT`.** The current OtaForwarder
phase enum (in `OtaForwarder.hpp`) gains a new state inserted between
`AWAITING_END_ACK` and `BETWEEN_PADAWANS`:

```cpp
enum class Phase : uint8_t {
    IDLE,
    AWAITING_BEGIN_ACK,
    STREAMING,
    AWAITING_END_ACK,
    AWAITING_FLASH_RESULT,   // NEW: holding for padawan's flash outcome
    BETWEEN_PADAWANS,
};
```

State transitions adjust: on `OTA_END_ACK OK` arrival, instead of
calling `completeCurrentPadawan` immediately, the forwarder transitions
to `AWAITING_FLASH_RESULT`, emits FW_PROGRESS FLASHING, and arms the
flash-result timer. `completeCurrentPadawan` runs on `OTA_FLASH_RESULT`
arrival (with the status mapped into `results_`) or on flash-result
timer fire (recording the `flash_result_timeout` failure).

Sequential per-padawan iteration (current model) means the next
padawan's deploy doesn't start until the current padawan's
flash-result is in. The flash-result timer prevents indefinite blocking
on a dead padawan.

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

The new wire message (`OTA_FLASH_RESULT`) and new server stage
(`FwStage.Flashing`) need to land together — a server running pre-
this-design code would still hit the illegal-transition bug, and an M4
firmware emitting FW_PROGRESS against a pre-this-design server would
have its messages logged as unknown. Coordination:

1. Land firmware wire-format additions first (new `OtaFlashStatus`
   enum, `OtaFlashResultPayload` struct, `OTA_FLASH_RESULT` packet
   type, native round-trip tests). No behavioral change yet — the new
   message is defined but never emitted or consumed. This step is
   independently mergeable.
2. Land server-side enum + state-machine + UI mapping changes
   (`FwStage.Flashing`, `Verifying → Flashing → Failed` transitions,
   `FLASHING → 'flash'` mapping, `FIRMWARE_STAGES` row reorder). M4
   firmware in the field continues to hit the illegal-transition bug
   (no regression).
3. Land firmware FW_PROGRESS emission + padawan flash-result emission
   change. M4 deploys against the updated server now show the intended
   UI flow.
4. When PR set 2 ships:
   - Padawan replaces the `FLASH_NOT_IMPLEMENTED` placeholder with the
     real `esp_ota_set_boot_partition` call and emits
     `OtaFlashResult(OK)` or `OtaFlashResult(FAILED)` per the IDF
     return.
   - Padawan adds the reboot trigger after a successful flash-result
     send.
   - Firmware adds `REBOOTING` + `VERSION_CONFIRMED` FW_PROGRESS
     emission (triggered by post-reboot heartbeat resync paths).
   - Server's `Flashing → Rebooting → VersionConfirmed` path becomes
     the happy path; `FLASH_NOT_IMPLEMENTED` retires (or stays as a
     debug/test-mode hook for development builds).

## Phasing

Single phase. ~8 tasks, two-repo coordination. At the "more than ~8
discrete tasks" scope-guard threshold from `CLAUDE.md` — manageable as
one plan but worth flagging. Tasks:

1. **AstrOs.ESP — `OTA_FLASH_RESULT` wire format.** Add
   `OtaFlashStatus` enum, `OtaFlashResultPayload` struct,
   `buildOtaFlashResult` / `parseOtaFlashResult`, new
   `AstrOsPacketType::OTA_FLASH_RESULT` variant, native round-trip
   tests. No emit/consume yet — wire format only.
2. **AstrOs.ESP — `getFwProgress` + `sendFwProgress`.** Builder +
   wrapper + native round-trip test.
3. **AstrOs.ESP — `OtaForwarder` FW_PROGRESS emission.** Wire
   SENDING/VERIFYING/FLASHING per the cadence table. VERIFYING fires
   on `emitOtaEndFrame`; FLASHING fires on `OTA_END_ACK OK` arrival.
4. **AstrOs.ESP — `OtaForwarder` flash-result handling.** Add
   consumer for `OTA_FLASH_RESULT`, map status into `results_`, add
   per-padawan flash-result wait timer (`kFlashResultTimeoutUs`,
   proposed 10 s).
5. **AstrOs.ESP — `OtaWriter::handleEnd` placeholder.** Send
   `OTA_END_ACK OK` immediately (unchanged), then `vTaskDelay(2 s)`,
   then send `OTA_FLASH_RESULT(FLASH_NOT_IMPLEMENTED, "pr_set_1_placeholder")`.
   Comment block referencing this design.
6. **AstrOs.Server — enum + state machine + mapping + row order.** Add
   `FwStage.Flashing`, update transitions (`Verifying → Flashing`,
   `Flashing → Rebooting`, `Flashing → Failed`), update
   `mapServerStageToUiStage` (`FLASHING → 'flash'`), update
   `controllerStatePillKind`, update `ServerFwStage` type, reorder
   `FIRMWARE_STAGES` to `['download', 'transfer', 'verify', 'flash', 'reboot']`,
   update affected tests.
7. **AstrOs.Server — orchestrator integration test.** Simulate M4
   sequence end-to-end (FW_PROGRESS chain + FW_DEPLOY_DONE FAILED with
   `flash_not_implemented`), assert UI-visible state and the
   2 s-visible-Flashing timing assumption.
8. **Bench validation.** 2-padawan deploy from the server UI must
   render `✓ download ✓ transfer ✓ verify ! flash idle-reboot` and
   surface the `flash_not_implemented` reason in the controller-detail
   view. Verify state visibly persists for ~2 s before flashing turns
   red.

## Related references

- M3 master-side design: `.docs/plans/20260523-1023-firmware-ota-mesh-forward-design.md`
- Original FW_PROGRESS cadence proposal: same file, lines 391-405
- Cross-repo decomposition: `AstrOs.Server/.docs/completed_plans/2026/04/27/20260427-2202-firmware-ota-decomposition.md`
- Server state machine: `astros_api/src/firmware/flash_job_state_machine.ts`
- Server FW_PROGRESS parser: `astros_api/src/serial/message_handler.ts:301-332`
- UI stage list: `astros_vue/src/components/firmware/firmwareStagesList/stageRowState.ts:3`
- UI stage mapping: `astros_vue/src/utils/firmwareStageMapping.ts`
