# OTA — Master Serial Receive QA

Manual QA plan for the ESP firmware OTA receive path. Phases referenced match
`.docs/plans/20260514-1941-firmware-ota-esp-master-serial-receive-design.md`.

## Phase 3 — Wire-up sink-to-/dev/null

### Preconditions

- Master flashed with `feature/ota-phase3-wire-up` (or `develop` once merged).
- AstrOs.Server `develop` running, pointed at the master's serial port.
- Any 1.2 MB `.bin` available — content is discarded in Phase 3 (Phase 4 will
  start hashing).
- SD card present is **not** required for Phase 3 — the receiver sinks payload
  bytes to /dev/null and never opens a file. Phase 4 adds the SD writer.

### Steps

1. Open the Firmware view in AstrOs.Server.
2. Upload the 1.2 MB `.bin`.
3. Click `Send to master`.

### Expected results

- Master serial log lines (timestamps and exact payloads will differ):

```
I OtaReceiver: OtaReceiver initialized                            (at boot)
...
I OtaReceiver: FW_TRANSFER_BEGIN accepted: transferId=<id> totalSize=1228800 chunks=300 sha=<hex>
                                                                  (no per-chunk INFO log)
I OtaReceiver: FW_TRANSFER_END OK: transferId=<id> totalChunks=300
I OtaReceiver: FW_DEPLOY_BEGIN stub: transferId=<id> target-count=<N> — all-FAILED not_implemented
```

- Server-side console: `transfer complete`, then `deploy failed: not_implemented`.
- JobLock releases — a subsequent non-OTA write (e.g. `RUN_SCRIPT`) succeeds.
- No `OTA Receiver Stack HWM:` warnings (would indicate the 4 KB task stack
  is too small; bump to 5120 or 6144 and re-test).
- No recurring `FW_CHUNK_NAK reason=CRC` lines — those would point at a base64
  decode or CRC verification bug.

### Negative path 1 — Duplicate BEGIN during active transfer

**Preconditions:** A transfer is in flight (BEGIN ACK received, chunks streaming).

**Steps:**
1. From a second terminal, send a second `FW_TRANSFER_BEGIN` with a different transferId.

**Expected:**
- `FW_TRANSFER_BEGIN_ACK status=busy transferId=<new-id>`.
- The in-flight transfer continues uninterrupted to its END.
- Master log: `FW_TRANSFER_BEGIN while transfer <running-id> active; replying busy`.

**Pass/Fail:** Pass if the original transfer reaches OK and no chunks are dropped.

### Negative path 2 — Chunk with no transfer active

**Preconditions:** No active transfer (master idle or post-END).

**Steps:**
1. Send a single `FW_CHUNK` line directly to the master serial port.

**Expected:**
- `FW_CHUNK_NAK reason=OUT_OF_ORDER lastGoodSeq=0 nextExpectedSeq=0`.
- Master log: `FW_CHUNK while no transfer active; emitting inactive NAK`.

**Pass/Fail:** Pass if a single NAK is emitted and no state change is logged.

### Negative path 3 — Malformed base64 payload

**Preconditions:** A transfer is in flight, chunks flowing.

**Steps:**
1. Intercept a `FW_CHUNK` line at the server and replace a single base64 char with `*`.
2. Forward the corrupted line; the rest of the transfer continues unchanged.

**Expected:**
- `FW_CHUNK_NAK reason=SIZE lastGoodSeq=<prev> nextExpectedSeq=<rejected seq>`.
- Master log at LOGE: `FW_CHUNK base64 invalid character (seq=<seq> transferId=<id>...)`.
- Server retransmits; transfer completes successfully.

**Pass/Fail:** Pass if the LOGE fires exactly once per corruption and the transfer recovers.

### Negative path 4 — Invalid BEGIN parameters

**Preconditions:** Master idle.

**Steps:**
1. Send `FW_TRANSFER_BEGIN` with `chunkSize=0`.
2. Send `FW_TRANSFER_BEGIN` with `totalChunks=0`.
3. Send `FW_TRANSFER_BEGIN` with `payloadLen=0` (via a hand-crafted `FW_CHUNK`).

**Expected:**
- Cases 1+2: `FW_TRANSFER_BEGIN_ACK status=io_error` from BulkReceiver rejection.
- Case 3: `FW_CHUNK` parser rejects at the wire layer — no malloc(0), no NAK with confusing severity.
- No transfer state is left active afterward.

**Pass/Fail:** Pass if `isActive()` returns false after each case (verify via subsequent BEGIN reaching OK).

### Negative path 5 — Stray END with mismatched transferId

**Preconditions:** A transfer is in flight (transferId=5, say).

**Steps:**
1. From a second terminal, send a `FW_TRANSFER_END transferId=99 totalChunks=300 sha=<any>`.

**Expected:**
- `FW_TRANSFER_END_ACK status=IO_ERROR transferId=99`.
- Master log: `WRONG_XFER_ID (transferId=99 server-state bug)`.
- The in-flight transfer (5) continues to its OK end. **State is not torn down by the stray END.**

**Pass/Fail:** Pass if the original transfer's END reaches OK after the stray END.

### Out of scope for Phase 3

- SHA-256 verification (Phase 4).
- SD persistence + content-addressed rename (Phase 4).
- Mesh forward to padawans + commit/reboot (out of Phase 1-5 entirely; future
  sub-project (b) work).
- `FW_BACKPRESSURE` PAUSE/RESUME flow (Phase 5, only built if Phase 3/4 measurements
  show the master falling behind).
- 5-minute whole-transfer watchdog (Phase 5, optional). The current 10 s idle
  watchdog catches "transfer goes silent" but not "transfer dribbles forever" —
  e.g., a CHUNK every 9 s indefinitely. Polling stays paused for the duration.
  The deadline-based watchdog will land alongside Phase 4 deploy/flash work where
  the abort semantics are decided together with disk-write timeouts.
