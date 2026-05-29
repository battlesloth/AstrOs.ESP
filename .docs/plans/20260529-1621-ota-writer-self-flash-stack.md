# Fix: ota_writer_task stack overflow during master self-flash

## Problem

Bench run (2026-05-29, metro_s3 / ESP32-S3) crashed with a FreeRTOS stack
overflow in `ota_writer_task` immediately after `startMasterSelfFlash` →
`OtaWriter::handleLocalFlashReq` began streaming a ~1.27 MB firmware image
from `/sdcard/firmware/…`:

```
***ERROR*** A stack overflow in task ota_writer_task has been detected.
  vApplicationStackOverflowHook → vTaskSwitchContext (mid-fread context switch)
```

### Root cause

`OtaWriter::handleLocalFlashReq` declares **two** 4 KB buffers at function
scope:

- `uint8_t buf[4096]` (`lib/OtaWriter/src/OtaWriter.cpp` Step 3) — file streaming
- `uint8_t rbBuf[4096]` (Step 6) — read-back verify

metro_s3 builds with `CONFIG_COMPILER_OPTIMIZATION_DEFAULT` (`-Og`). GCC's
`-fstack-reuse` only reclaims a slot when a variable's *scope* ends; both
arrays are function-scoped, so they coexist for the whole call = **8192 bytes
of arrays alone** on an **8192-byte** task stack (`ota_writer_task`,
`src/main.cpp:248`). Add the `AstrOsSha256Ctx rbCtx` (108 B), digests, the
`postResult` lambda frame, and the deep `fread → FATFS → SD/SPI` call chain
layered on top during Step 3 → guaranteed overflow.

The padawan receive path survives the same 8 KB stack because `handleData`
writes ~180 B chunk payloads straight from the queue (no big array) and
`handleEnd` uses exactly **one** 4 KB buffer reading via the shallow
`esp_partition_read` (not FATFS/SD). The self-flash path both doubled the
buffers and swapped in a deeper read path.

The 500-byte high-water-mark warning (`src/main.cpp:1618`) never fired: it runs
*after* `process()` returns, between messages, so it is structurally blind to a
single handler that overruns mid-execution.

## Approach (Option A)

1. **Collapse the two 4 KB buffers into one reused buffer.** Step 3 and Step 6
   run strictly sequentially (the file is `fclose`'d before readback begins),
   so a single `buf[4096]` is provably safe and mirrors the proven-good
   `handleEnd` single-buffer pattern. Halves buffer footprint to 4 KB.
2. **Bump `ota_writer_task` stack 8192 → 12288.** Gives margin for the
   `fread → FATFS → SD/SPI` call depth that the receive path never exercises
   and that cannot be measured precisely from source. RAM is plentiful
   (~112 KB free on the bench run); +4 KB stack is comfortable.

Net permanent RAM cost: +4 KB. No behavior change beyond stack/buffer layout.
Not unit-testable (stack depth is invisible to host tests); build checks pass,
and master self-flash still needs a bench re-run.

## Tasks

- [x] Collapse Step 6's `rbBuf`/`kRbBufSize` to reuse Step 3's `buf`/`kChunkBytes` in `lib/OtaWriter/src/OtaWriter.cpp`; add a comment noting the single-buffer stack budget
- [x] Update the `handleEnd` readback comment that references "otaWriterTask's stack (8 KB)" to "(12 KB)"
- [x] Bump `ota_writer_task` stack 8192 → 12288 in `src/main.cpp:248` and refresh the sizing comment to mention the self-flash FATFS/SD read depth
- [x] Build + tests: `pio test -e test` 482/482 pass; `pio run -e metro_s3` and `pio run -e lolin_d32_pro` both SUCCESS; clang-format clean on both changed files
- [ ] Commit; bench-verify master self-flash on hardware (manual, off-plan)
