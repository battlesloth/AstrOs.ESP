# astrosRxTask — compact log for FW_CHUNK lines

## Problem

`astrosRxTask` logs every assembled line at INFO with the full buffer via `%s`. After bumping the buffer to 8 KB, this now dumps ~5.5 KB of base64 to the console per chunk — ~300 chunks × 5.5 KB ≈ 1.6 MB of log output per flash. The log shares UART0 TX with FW_CHUNK_ACK; a 5.5 KB log line takes ~480 ms to emit at 115200 baud, which can starve the server's sliding window and trip its retransmit timer (undoing the buffer-bump fix). The monitor console also becomes unreadable during a flash.

## Approach

Detect FW_CHUNK lines (bounded scan of the first ~32 bytes for the literal `"FW_CHUNK"`) and log a compact one-liner with the byte count + the trailing 4-char CRC instead of the full buffer. The CRC is the last 4 chars of the line by protocol contract (FW_CHUNK ends with `<US>crc16hex`), and the byte count is already in `bufferIndex` — no parsing of intermediate fields required. Control messages (POLL_ACK, REGISTRATION_SYNC, FW_TRANSFER_BEGIN/END, FW_DEPLOY_BEGIN) keep the existing full-line log, preserving their diagnostic value.

False-positive scan: only inbound messages traverse this RX task, and no other inbound type name starts with or contains `"FW_CHUNK"` (FW_CHUNK_ACK / FW_CHUNK_NAK are outbound from the master, never received here). FW_TRANSFER_* and FW_DEPLOY_BEGIN have distinct names. Safe to substring-match.

## Tasks

- [ ] **Branch the log in `astrosRxTask`.** Bounded scan over `commandBuffer[0..min(32, bufferIndex))` for `"FW_CHUNK"`. On match: `ESP_LOGI("AstrOs RX", "FW_CHUNK (%d bytes, crc=%.4s)", bufferIndex, &commandBuffer[bufferIndex - 4])`. On no match (or buffer too short to contain a CRC): existing full-line log. Comment the why so a future reader doesn't undo the special case.

- [ ] **Build + bench verify.** Both boards. Reflash master. Run a flash; expect compact `FW_CHUNK (5530 bytes, crc=1b44)` lines instead of base64 dumps, AND ACKs flowing in time so the streamer doesn't retransmit.

## Files touched

- `src/main.cpp` — `astrosRxTask` log branch

## Out of scope

- **Dropping the per-line LOGI entirely** (Option B from the prior discussion). Keeping the compact-FW_CHUNK + full-other-message split preserves diagnostic value for control messages while removing the chunk-flooding amplification.
- **Logging seq + payloadLen** in addition to CRC. Both would require parsing intermediate `<US>`-delimited fields, adding complexity for marginal extra diagnostic value (the bench QA reproductions can correlate seq via the server log).
- **A per-chunk DEBUG-level diagnostic** for deep debugging. If a future investigation needs full-chunk visibility, the temporary fix is to flip the log level or undo this branch — not a permanent code path worth maintaining.
