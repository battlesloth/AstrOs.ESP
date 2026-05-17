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
I OtaReceiver: FW_DEPLOY_BEGIN: transferId=<id> target-count=<N> — all-FAILED not_implemented
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

### Negative path 4 — Invalid transfer parameters

**Preconditions:** Master idle.

**Steps:**
1. Send `FW_TRANSFER_BEGIN` with `chunkSize=0`.
2. Send `FW_TRANSFER_BEGIN` with `totalChunks=0`.
3. Send a hand-crafted `FW_CHUNK` with `payloadLen=0`.

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
- Master log (LOGE): `FW_TRANSFER_END IO_ERROR reason=WRONG_XFER_ID (transferId=99 server-state bug)`.
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

## Phase 4 — SD writer + streaming SHA-256

### Preconditions

- Master flashed with `feature/ota-phase4-sd-writer-sha256` (or `develop` once merged).
- AstrOs.Server `develop` running, pointed at the master's serial port.
- A known-good 1.2 MB `.bin`. Record its SHA-256 out-of-band:
  `sha256sum firmware.bin` → save the hex digest.
- SD card present, FAT-formatted, with at least ~5 MB free (well above the
  1.2 MB pre-check threshold). The receiver auto-creates `/sdcard/firmware/`
  on the first BEGIN.

### Happy-path steps

1. From the AstrOs.Server Firmware view, upload the recorded `.bin`.
2. Click `Send to master`.
3. After the UI reports "transfer complete", power down the master and pull
   the SD card.
4. On the host: `ls /sdcard/firmware/` and `sha256sum
   /sdcard/firmware/<sha-prefix>.bin`.

### Expected results

- Master serial log (timestamps and exact values will differ):

```
I OtaReceiver: FW_TRANSFER_BEGIN accepted: transferId=<id> totalSize=1228800 chunks=300 sha=<full-hex>
I OtaReceiver: FW_TRANSFER_END OK: transferId=<id> totalChunks=300 path=/sdcard/firmware/<sha-prefix>.bin sha=<full-hex>
I OtaReceiver: FW_DEPLOY_BEGIN: transferId=<id> target-count=<N> — all-FAILED not_implemented
```

- The END_ACK now carries the **locally computed** hash, not the server's echo.
  The server compares it to its own digest and reports OK if they match.
- SD inspection:
  - `staging.bin` is **absent** (renamed on success).
  - A single file named `<first-16-hex-chars-of-sha>.bin` exists.
  - `sha256sum` of that file matches the value recorded in step 0 byte-for-byte.
- No `OTA Receiver Stack HWM:` warning (would mean the 4 KB task stack is
  too small after adding the on-stack `mbedtls_sha256_context`; bump to 5120
  if it fires).

### Negative path 6 — Watchdog discards staging.bin on stuck transfer

**Preconditions:** A transfer is in flight (BEGIN ACK received, chunks streaming),
SD card contains an in-progress `staging.bin`.

**Steps:**
1. Pull the serial cable mid-transfer.
2. Wait ≥10 s for the idle watchdog to fire (visible in logs as
   `OTA watchdog fired: transferId=<id> idle >10000ms — aborting transfer`).
3. Reattach the serial cable.
4. From the server, upload again with a different `.bin`.

**Expected:**
- After step 2, the watchdog log line appears and the receiver returns to idle
  (`isActive()` becomes false). Note: confirming `staging.bin` is absent here
  requires removing the SD card, which interrupts the test — defer the on-disk
  check to step 5 below if needed.
- The second BEGIN in step 4 is accepted cleanly (no `busy` reject).
- The second transfer completes with its own `<sha-prefix>.bin` on the SD card.

**Pass/Fail:** Pass if (a) the watchdog log fires within ~11 s, (b) the second
upload completes OK, and (c) the SD card eventually shows only the **second**
firmware's `<sha-prefix>.bin` (no leftover from the first attempt — the
watchdog's `keepStaging=false` policy removes it).

### Negative path 7 — Same-firmware re-upload is idempotent

**Preconditions:** A successful Phase 4 transfer has already landed
`/sdcard/firmware/<sha-prefix>.bin` on the card.

**Steps:**
1. Without changing the `.bin`, upload it again from the server.

**Expected:**
- BEGIN_ACK OK, normal chunk flow, END_ACK OK with the same computed hash.
- On the SD card after success: still exactly one `<sha-prefix>.bin` with the
  same name and same contents. The `unlink` before `rename` makes the
  same-path-target case work; no `staging.bin` left behind.

**Pass/Fail:** Pass if the second upload completes cleanly and the SD card
state is unchanged.

### Negative path 8 — HASH_MISMATCH preserves staging.bin

**Preconditions:** Master idle, SD card empty of any prior `/sdcard/firmware/`
contents.

**Steps:**
1. From the server, drive a transfer with a deliberately wrong END hash. The
   simplest path is a server-side debug toggle that flips one byte of
   `finalSha256Hex` in the `FW_TRANSFER_END` frame before sending; if the
   toggle doesn't exist yet, hand-craft the frame from a serial console
   after recording the real chunk stream.
2. Watch the master serial log.
3. Power the master down, pull the SD card.

**Expected:**
- Master serial log includes a single LOGW like
  `FW_TRANSFER_END HASH_MISMATCH: transferId=<id> computed=<a> expected=<b> — staging.bin preserved`.
- `FW_TRANSFER_END_ACK status=HASH_MISMATCH computedHex=<a>` reaches the
  server.
- SD inspection:
  - `/sdcard/firmware/staging.bin` is **present** and its size equals
    `totalSize` from BEGIN.
  - **No** `<computed-prefix>.bin` or `<expected-prefix>.bin` was created.
- A subsequent BEGIN truncates `staging.bin` cleanly — verify by uploading
  the real firmware next and confirming the rename to `<prefix>.bin`
  succeeds and `staging.bin` is gone afterward.

**Pass/Fail:** Pass if the HASH_MISMATCH log fires, `staging.bin` survives
on disk, and no renamed file was created. This is the **only** path that
exercises the forensic-preserve contract — a regression here (e.g., an
errant `resetCryptoAndFile(false)` on the HASH_MISMATCH branch) would
silently destroy evidence operators rely on for post-mortem investigation.

### Out of scope for Phase 4 (deferred to Phase 5)

- Pre-filled SD card to force `sd_full` — verifies the BEGIN free-space gate.
- Mid-transfer CRC injection persisting across retries — verifies the
  receiver still completes after server-side retransmits.
- 5-min whole-transfer wall-clock deadline. The Phase 3 idle watchdog covers
  the dominant failure mode; the deadline backstop will be evaluated against
  measured throughput before being built.
