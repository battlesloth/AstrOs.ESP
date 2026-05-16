# astrosRxTask command buffer too small for FW_CHUNK

## Problem

`astrosRxTask` in `src/main.cpp` assembles incoming serial bytes into newline-terminated lines via a 2000-byte `commandBuffer`. When a line exceeds the buffer, the overflow handler at line 1034-1039 **resets `bufferIndex` to 0 and continues appending**, so the tail of the line clobbers the head. The next `\n` then hands `handleMessage` a corrupted partial message starting somewhere mid-base64. Result: every FW_CHUNK is rejected as "Invalid message" and the master never ACKs, even though the CRC is now correct.

Protocol sizing:
- Serial chunk size: **4096 bytes (decoded)** per `AstrOs.ESP/.docs/protocol.md` "Shared values"
- Base64-encoded: `ceil(4096 × 4/3)` ≈ 5462 bytes
- FW_CHUNK line: header + UUID msgId + GS + transferId + US + seq + US + payloadLen + US + base64 + US + crc16hex + EOL ≈ **5530 bytes**

2000 < 5530. Inevitable corruption on every chunk.

Pre-existing latent gap from c.6c.1 — FW_CHUNK was added without bumping `bufferLength`. No earlier protocol message exceeded 2KB so it didn't surface until OTA testing.

## Approach

Single constant bump:

```cpp
size_t bufferLength = 8192;
```

8KB comfortably holds a FW_CHUNK line (~5.5KB) with headroom for the absolute max-size case (transferId at 3 chars, seq at 4 chars, etc.), AND leaves room for future protocol messages without re-touching this. ESP32 heap is ample (~300KB free); a one-time 8KB malloc per task is negligible.

Leave the overflow handler as-is (still resets to 0 on overflow, which is the right behavior for unexpected protocol violations — a 9KB line is broken protocol, not a buffer-too-small case). The warn log + counter increment stay; both are diagnostic for "this should never happen" once the bump is in place.

## Tasks

- [x] **Bump `bufferLength` in `astrosRxTask`.** From 2000 to 8192. Update the inline comment to document the FW_CHUNK sizing requirement so a future reader knows why it's larger than the buffer-size-vs-stack-size heuristic would suggest.

- [x] **Audit other line-assembly paths.** Quick grep for similar `bufferIndex >= bufferLength` patterns in `lib/` — confirm no other serial-receive path has the same too-small-for-FW_CHUNK ceiling. If found, fix or note as a follow-up.

- [x] **Build + bench verification.** Both board builds; reflash the master; retry the flash. Expected: no `AstrOs RX: Buffer overflow` warnings, no `Invalid message` errors on FW_CHUNK lines, master ACKs chunks normally, transfer completes.

## Files touched

- `src/main.cpp` — `astrosRxTask`'s `bufferLength`

## Out of scope

- **Dynamic buffer reallocation** (start small, grow on demand). Static allocation is simpler and ESP32 has the heap headroom. Revisit only if a future protocol message exceeds 8KB.
- **UART driver-side ring buffer size** (`RX_BUF_SIZE * 2 = 2048` at line 274). uart_read_bytes drains 1KB per call at 115200 baud (~90ms per fill); the task keeps up. Driver-side overflows would produce different symptoms (uart_event UART_FIFO_OVF) which we're not seeing. Leave alone.
- **Reducing the server's chunk size** to fit within 2KB master buffer. Wrong direction — protocol-doc canonical is 4096 decoded bytes, and smaller chunks would 5× the per-chunk overhead without solving the underlying issue (a future message could still exceed any small cap).
