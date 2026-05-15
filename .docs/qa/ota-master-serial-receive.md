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

### Negative paths exercised in Phase 3

These are inherent to the wire-up rather than requiring a special server build:

- Server sends BEGIN while a transfer is already active →
  `FW_TRANSFER_BEGIN_ACK status=busy`, in-flight transfer continues unaffected.
- Server sends a chunk while no transfer is active →
  `FW_CHUNK_NAK reason=OUT_OF_ORDER lastGoodSeq=0 nextExpectedSeq=0`.
- Server sends a malformed base64 payload →
  `FW_CHUNK_NAK reason=SIZE lastGoodSeq=0 nextExpectedSeq=<rejected seq>`.
- Server sends BEGIN with `chunkSize=0` or `totalChunks=0` (BulkReceiver validation
  rejection) → `FW_TRANSFER_BEGIN_ACK status=io_error`.

### Out of scope for Phase 3

- SHA-256 verification (Phase 4).
- SD persistence + content-addressed rename (Phase 4).
- Mesh forward to padawans + commit/reboot (out of Phase 1-5 entirely; future
  sub-project (b) work).
- `FW_BACKPRESSURE` PAUSE/RESUME flow (Phase 5, only built if Phase 3/4 measurements
  show the master falling behind).
- 5-minute whole-transfer watchdog (Phase 5, optional).
