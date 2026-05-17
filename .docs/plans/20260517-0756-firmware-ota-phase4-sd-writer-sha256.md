# OTA Phase 4 — SD writer + streaming SHA-256 (plan)

## Context

Phases 1-3 of the firmware OTA feature shipped through PR #32 (May 16) plus three
post-merge hardening commits (poll-gating, idle watchdog, PR-review fixes). The
end-to-end serial pipeline is working: server can drive a 1.2 MB transfer, the
master's `OtaReceiver` consumes `OTA_MSG_BEGIN`/`CHUNK`/`END`/`DEPLOY_BEGIN`,
the `BulkReceiver` validates each chunk's CRC and sequence, and ACK/NAK replies
flow back. But every payload byte is currently discarded — the receiver acts
as a `/dev/null` sink.

Phase 4 makes the master actually keep what it receives. It adds two pieces of
streaming state to `OtaReceiver` (`FILE *staging_` and `mbedtls_sha256_context
shaCtx_`), drives them from the existing BEGIN/CHUNK/END handlers, and on a
clean END renames the staged file to its content-addressed final name so it
survives reboot and can be consumed by the future mesh-forwarder phase. The
design contract is frozen in `.docs/plans/20260514-1941-firmware-ota-esp-master-serial-receive-design.md`
(§ "Phase 4 — SD writer + streaming SHA-256"); this plan implements it without
amendment.

User-confirmed decisions (2026-05-17):
- Scope matches the design exactly. Wall-clock deadline, FW_BACKPRESSURE, and
  full negative-path QA stay deferred to Phase 5.
- Final filename is content-addressed: `/sdcard/firmware/<sha256-hex[0..16)>.bin`.

## Critical files

**Modified:**
- `lib/OtaReceiver/include/OtaReceiver.hpp` — add `FILE *staging_`,
  `mbedtls_sha256_context shaCtx_`, `bool shaActive_`, plus private helpers
  `openStaging()`, `closeStaging(bool keep)`, `resetCryptoAndFile()`.
- `lib/OtaReceiver/src/OtaReceiver.cpp` — wire the three state-machine handlers
  to file + SHA operations and to the new status codes. The `handleEnd` OK
  branch now emits the *locally computed* hash, not the echoed `finalShaIn`.
- `lib/AstrOsStorageManager/include/AstrOsStorageManager.hpp` — declare
  `uint64_t freeSpaceSdBytes()` and `bool ensureSdFirmwareDir()`.
- `lib/AstrOsStorageManager/src/AstOsStorageManager.cpp` — implement both
  helpers. `freeSpaceSdBytes` wraps `f_getfree` (the FATFS card pointer is
  already held at file scope as `static sdmmc_card_t *card`).
  `ensureSdFirmwareDir` calls `mkdir("/sdcard/firmware", 0775)` and treats
  `EEXIST` as success.

**Unchanged but load-bearing — read before editing:**
- `.docs/plans/20260514-1941-firmware-ota-esp-master-serial-receive-design.md`
  §§ "Phase 4", "Buffer ownership", "Error handling matrix" — the contract.
- `lib/AstrOsSerialMsgHandler/src/AstrOsSerialMsgHandler.cpp:317-322` —
  `sendFwTransferEndAck(msgId, transferId, status, computedSha256Hex)` already
  takes the hash; Phase 4 just stops passing the server's hash and starts
  passing the local one.
- `lib_native/AstrOsBulkTransport/.../AstrOsBulkTransport.hpp` —
  `ChunkResult.payload`/`payloadLen` are the validated bytes we write.
- `lib/OtaReceiver/include/OtaQueueMessage.h:39-46` — `SHA256_HEX_LEN = 64`
  inline buffer convention.
- `lib/OtaReceiver/README` — already states the lib lands `<mbedtls/sha256.h>`
  and is MIXED; no classification change needed.

## Approach

### State held across chunks of one transfer

```cpp
// OtaReceiver.hpp additions (private)
FILE *staging_ = nullptr;          // open between BEGIN OK and END / abort
mbedtls_sha256_context shaCtx_;    // raw ctx; init'd lazily in handleBegin
bool shaActive_ = false;           // tracks whether shaCtx_ needs mbedtls_sha256_free
```

The pair `(staging_, shaCtx_)` is *only* mutated on `otaReceiverTask` (matches
the existing single-task-state invariant in the header docstring). Every exit
path goes through `resetCryptoAndFile()` so abort/success/watchdog-fire all
land in the same clean state.

### Per-handler changes

**`handleBegin`** — after the existing busy/parse-strict-u8/BulkReceiver
`.begin()` checks succeed, in order:

1. `AstrOs_Storage.ensureSdFirmwareDir()` — fail → `io_error`.
2. `uint64_t freeBytes = AstrOs_Storage.freeSpaceSdBytes()` — if
   `freeBytes < msg.begin.totalSize` reply `sd_full` (new status string —
   already in the wire contract per `.docs/protocol.md`; handler-side string
   constant added if missing).
3. `staging_ = fopen("/sdcard/firmware/staging.bin", "wb")` — fail →
   `io_error`. The `"wb"` mode truncates so a leftover from a prior
   `HASH_MISMATCH` is replaced cleanly.
4. `mbedtls_sha256_init(&shaCtx_); mbedtls_sha256_starts(&shaCtx_, 0);
   shaActive_ = true;`
5. Only now flip `active_ = true`, send `BEGIN_ACK status=OK`, start watchdog.

Order matters: nothing past step 1 mutates `active_`, so any early-return
leaves `OtaReceiver` in the same idle state it was in before BEGIN arrived.

**`handleChunk`** — after `bulk_.onChunk` returns ACK and *before* sending the
`FW_CHUNK_ACK`:

```cpp
size_t w = fwrite(cr.payload, 1, cr.payloadLen, staging_);
if (w != cr.payloadLen) {
    ESP_LOGE(TAG, "fwrite short: wrote=%zu of %u — aborting transfer", w, cr.payloadLen);
    AstrOs_SerialMsgHandler.sendFwChunkNak(transferIdIn, cr.highestContiguousSeq,
                                           cr.nextExpectedSeq, "FLASH_FULL");
    resetCryptoAndFile(/*keepStaging=*/false);
    bulk_.reset(); active_ = false; transferIdStr_.clear(); watchdogStop();
    return;
}
mbedtls_sha256_update(&shaCtx_, cr.payload, cr.payloadLen);
```

The fwrite-failure path mirrors the design's error-handling matrix:
`FW_CHUNK_NAK reason=FLASH_FULL` and the transfer goes inactive. The server's
next BEGIN starts a fresh staging.bin (truncating mode). NAK path on bad CRC
is unchanged — no write, no SHA update.

**`handleEnd`** — replace the current "echo server's hash" line. When
`bulk_.onEnd` returns OK:

```cpp
// Close and flush before hashing — fclose drains stdio buffers.
if (fclose(staging_) != 0) { /* IO_ERROR + computedHex="" + cleanup */ }
staging_ = nullptr;

uint8_t digest[32];
mbedtls_sha256_finish(&shaCtx_, digest);
mbedtls_sha256_free(&shaCtx_);
shaActive_ = false;

char computedHex[SHA256_HEX_LEN + 1];
toHexLower(digest, 32, computedHex);  // small static helper in this TU

if (strcmp(computedHex, msg.end.finalSha256Hex) == 0) {
    char finalPath[64];
    snprintf(finalPath, sizeof(finalPath), "/sdcard/firmware/%.16s.bin", computedHex);
    unlink(finalPath);  // tolerate same-firmware re-upload
    if (rename("/sdcard/firmware/staging.bin", finalPath) != 0) {
        // Rename failure is the only post-verify error path. Keep staging.bin
        // so a follow-up phase can still consume it. Reply IO_ERROR.
        AstrOs_SerialMsgHandler.sendFwTransferEndAck(msgId, transferIdIn, "IO_ERROR", computedHex);
    } else {
        ESP_LOGI(TAG, "FW_TRANSFER_END OK: %s sha=%s", finalPath, computedHex);
        AstrOs_SerialMsgHandler.sendFwTransferEndAck(msgId, transferIdIn, "OK", computedHex);
    }
} else {
    ESP_LOGW(TAG, "FW_TRANSFER_END HASH_MISMATCH: got=%s expected=%s — staging.bin preserved",
             computedHex, msg.end.finalSha256Hex);
    AstrOs_SerialMsgHandler.sendFwTransferEndAck(msgId, transferIdIn, "HASH_MISMATCH", computedHex);
    // Do NOT unlink staging.bin — forensic preservation per design.
}
```

`bulk_.onEnd` returning anything but OK keeps the existing IO_ERROR path, but
now also routes through `resetCryptoAndFile(/*keepStaging=*/true)` so the file
handle is closed and SHA context freed.

**`handleWatchdogFire`** — append `resetCryptoAndFile(/*keepStaging=*/false)`
before the existing `active_ = false; transferIdStr_.clear();`. Discarding
staging.bin is correct here because no END was seen and the bytes are
unverified.

**`handleBegin` (busy path) and parse-rejects** — must NOT touch `staging_` /
`shaCtx_`. Those belong to the in-flight transfer that's still running.

### Cleanup helper

```cpp
void OtaReceiver::resetCryptoAndFile(bool keepStaging) {
    if (staging_ != nullptr) {
        fclose(staging_);
        staging_ = nullptr;
        if (!keepStaging) unlink("/sdcard/firmware/staging.bin");
    }
    if (shaActive_) {
        mbedtls_sha256_free(&shaCtx_);
        shaActive_ = false;
    }
}
```

Idempotent. Called from `handleEnd` (both branches), `handleChunk` fwrite-fail
branch, `handleWatchdogFire`, and the destructor (with `keepStaging=true` —
shutdown is not a transfer outcome).

### StorageManager helpers

`freeSpaceSdBytes()` — use `f_getfree("0:", &nclust, &fs)` where `fs` is the
FATFS pointer reachable through the existing mounted card; multiply
`nclust * fs->csize * fs->ssize`. Returns `0` if SD is unmounted (forces the
caller into `io_error` rather than a false-positive "plenty of room").

`ensureSdFirmwareDir()` — `mkdir("/sdcard/firmware", 0775) == 0 || errno == EEXIST`.
Idempotent so it can be called every BEGIN without conditionalizing on a
"first transfer" flag.

## Reused, not re-implemented

- `AstrOs_Storage` (singleton in `lib/AstrOsStorageManager`) — already mounts
  the SD card at boot and exposes the `card` pointer needed for `f_getfree`.
- `mbedtls/base64.h` is already linked via
  `lib/AstrOsSerialMsgHandler/src/AstrOsSerialMsgHandler.cpp:10` — same
  mbedtls component covers `<mbedtls/sha256.h>` without platformio.ini
  changes.
- `sendFwTransferBeginAck` / `sendFwTransferEndAck` / `sendFwChunkNak`
  signatures already carry the fields Phase 4 needs (status strings, computed
  hash). No wire-format or builder changes.
- `BulkReceiver`'s `ChunkResult.payload` / `payloadLen` already deliver
  validated bytes — Phase 4 just consumes them. No PURE-lib changes.

## Task checklist

- [ ] Task 1 — Add `freeSpaceSdBytes()` and `ensureSdFirmwareDir()` to
      `AstrOsStorageManager` (header + impl). Log on failure; return safe
      defaults.
- [ ] Task 2 — Extend `OtaReceiver.hpp` with `staging_`, `shaCtx_`,
      `shaActive_`, and the private `resetCryptoAndFile` helper. Add
      `<mbedtls/sha256.h>` and `<cstdio>` includes.
- [ ] Task 3 — Rewrite `handleBegin` to add the ensure-dir / free-space /
      fopen / sha-start sequence. Map failures to the documented
      `io_error` / `sd_full` / `busy` status strings.
- [ ] Task 4 — Rewrite `handleChunk` ACK branch to `fwrite` + `sha256_update`.
      Add the `FLASH_FULL` NAK path with full state teardown.
- [ ] Task 5 — Rewrite `handleEnd` OK branch: `fclose`, `sha256_finish`, hex
      encode, content-addressed rename (with `unlink` of target first),
      `HASH_MISMATCH` preserves staging.bin. Wire all paths through
      `resetCryptoAndFile`.
- [ ] Task 6 — Update `handleWatchdogFire` (and any other abort surfaces) to
      call `resetCryptoAndFile(false)`. Audit the destructor.
- [ ] Task 7 — Extend `.docs/qa/ota-master-serial-receive.md` with the Phase 4
      "pop SD, verify on-disk SHA-256" expectation (per design § Phase 4
      review checkpoint). Bump `otaReceiverTask` stack size only if the
      bench run produces the HWM warning.

## Verification

**Native unit tests** (`pio test -e test`):
- No new PURE-lib code in this phase. Existing `bulk_transport_tests.cpp` and
  `astros_serial_messages_tests.cpp` must continue to pass — they cover the
  CRC, sequence, and wire-format contracts Phase 4 depends on. Expect 293+
  tests, 100% pass.

**Build matrix:**
- `pio run -e metro_s3` clean.
- `pio run -e lolin_d32_pro` clean.
- `clang-format` clean on changed files.

**Bench QA (Phase 4 review checkpoint, mirrors design §):**
1. Flash master with this branch on `metro_s3`. SD card inserted, formatted FAT.
2. From `AstrOs.Server` Firmware view, upload a known-good `.bin` whose
   SHA-256 is recorded out-of-band (`sha256sum file.bin`).
3. Expected serial-log flow:
   `BEGIN accepted → CHUNK_ACK ×N → END_ACK status=OK computedHex=<full-hash>
   → DEPLOY_DONE all-FAILED not_implemented` (DEPLOY stub from Phase 3 is
   unchanged).
4. Pull SD card. On the host: `ls /sdcard/firmware/` shows
   `<sha-prefix>.bin` and no `staging.bin`. `sha256sum <sha-prefix>.bin`
   matches the recorded value byte-for-byte.
5. Negative spot-check (not the full Phase 5 matrix): unplug serial mid-transfer.
   After ≥10 s watchdog fire shows in logs and `staging.bin` is gone (because
   `keepStaging=false` on watchdog abort).
6. Re-run the upload immediately after step 5 — verify the master accepts
   the new BEGIN cleanly (state was reset) and the second upload completes.

**Stack-pressure check:** watch for the "500 bytes remaining" HWM warning on
`otaReceiverTask` during a full 1.2 MB transfer. The new on-stack
`mbedtls_sha256_context` (~108 bytes) + temporary `digest[32]` /
`computedHex[65]` / `finalPath[64]` are small, but the current 4 KB stack
already runs Phase 3's BulkReceiver path; if the warning fires, bump to 5 KB
in `main.cpp`'s `xTaskCreatePinnedToCore` call (separate commit, justified
by observed HWM).

## Out of scope (deferred to Phase 5)

- Wall-clock max-transfer deadline (5-min `esp_timer` backstop). The idle
  watchdog from Phase 3 already covers the dominant failure mode.
- `FW_BACKPRESSURE PAUSE/RESUME`. Only build if SD-write throughput
  measurements during Phase 4 QA show the master falling behind.
- Full negative-path QA matrix (server-side wrong-sha injection, pre-fill SD
  for `sd_full`, mid-transfer CRC injection). Spot-check above proves the
  cleanup paths; the systematic matrix is Phase 5's job.
- Cleanup/eviction of stale `<sha-prefix>.bin` files. Owned by the future
  mesh-forwarder phase that first consumes them.
- ESP-NOW OTA packet types and any `esp_ota_*` calls. Belongs to the
  cross-repo sub-project (b) phase 2, not this milestone.
