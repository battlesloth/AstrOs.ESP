# AstrOs Firmware-OTA Protocol Contract

This document is checked into both **AstrOs.Server** and **AstrOs.ESP** at the same content. Any change must be made in both repos at the same commit; sub-projects code against this file. Do not let the two copies drift.

It covers the two cross-repo wire surfaces:

- **A.** Server ↔ Master ESP, over serial.
- **B.** Master ESP ↔ Padawan ESPs, over ESP-NOW.

The Server ↔ Vue WebSocket contract (`flashJobStarted`, `flashControllerUpdate`, etc.) lives in AstrOs.Server only; see `.docs/plans/20260427-2202-firmware-ota-decomposition.md` § "Server ↔ Vue".

## Reserved enum ranges

| Range | Owner | Notes |
|---|---|---|
| `SerialMessageType` 30–40 | This contract | `FW_*` messages, table A below. Last existing entry on Server: `SERVO_TEST_ACK = 22`; gap 23–29 reserved for in-flight non-OTA additions. |
| `AstrOsPacketType OTA_*` block | This contract | New packet types listed in table B below. Append to the existing enum; do not renumber existing entries. |

When extending either enum, **append** — never reorder existing entries. Both repos must update their copy of this file in lockstep.

## Shared values

| Item | Value | Notes |
|---|---|---|
| Image hash | SHA-256, hex-encoded as 64 lowercase chars | Computed by server over the full `.bin`; verified at master (after SD landing) and at each padawan (after `esp_ota_end`). |
| Frame CRC | CRC-16/CCITT-FALSE (poly `0x1021`, init `0xFFFF`) | Per ESP-NOW data frame, over `[transfer-id .. payload bytes]`. ESP-IDF: `esp_crc16_le`. |
| Serial chunk size | 4096 bytes (decoded) | Base64-encoded in the wire form, ≈ 5.5 KB per `FW_CHUNK` line at 115200 baud (~0.5 s transit). |
| ESP-NOW chunk size | 128 bytes | Per `OTA_DATA` frame; 1.2 MB image ≈ 9400 frames per padawan. |
| Serial sliding window | 16 frames | Inflight bytes ≈ 64 KB. |
| ESP-NOW sliding window | 8 frames | Inflight bytes ≈ 1 KB. |

## Stage enum (shared)

Master emits these in `FW_PROGRESS.stage` (table A). Server forwards them to the UI via `flashControllerUpdate`. Both sides must agree on the spelling.

```
QUEUED | UPLOADING_TO_MASTER | SENDING | VERIFYING | REBOOTING | VERSION_CONFIRMED | FAILED
```

## A. Server ↔ Master (serial)

Existing framing is unchanged: `[type(int)][RS][validator-string][RS][msg-id][GS][payload]\n` with `RS=0x1E`, `US=0x1F`, `GS=0x1D`. Chunk payloads are base64-encoded because the line-delimited framing breaks on raw binary.

### Message types

| Value | Name | Direction | Purpose |
|---|---|---|---|
| 30 | `FW_TRANSFER_BEGIN` | server → master | Announce job: targets, total size, expected SHA-256, chunk size. |
| 31 | `FW_TRANSFER_BEGIN_ACK` | master → server | Ready or reject (e.g. SD full). |
| 32 | `FW_CHUNK` | server → master | One frame of base64-encoded bytes with seq + CRC-16. |
| 33 | `FW_CHUNK_ACK` | master → server | Cumulative ACK with `next-expected-seq` and `window-remaining`. |
| 34 | `FW_CHUNK_NAK` | master → server | Bad CRC / out-of-order, with `last-good-seq`. |
| 35 | `FW_TRANSFER_END` | server → master | End-of-stream + final SHA-256. |
| 36 | `FW_TRANSFER_END_ACK` | master → server | `OK` / `HASH_MISMATCH` / `IO_ERROR` + computed hash. |
| 37 | `FW_DEPLOY_BEGIN` | server → master | Push staged SD copy to listed controllers, in given order. |
| 38 | `FW_PROGRESS` | master → server (unsolicited) | Per-controller, per-stage update. |
| 39 | `FW_DEPLOY_DONE` | master → server | All targets attempted; final per-controller report. |
| 40 | `FW_BACKPRESSURE` | master → server | Pause / resume sender. |

### Payload field layouts

`US` separates fields; `RS` separates repeated groups.

```
FW_TRANSFER_BEGIN:    transfer-id<US>total-size<US>sha256-hex<US>chunk-size<US>target-list
                      target-list = controllerId[RS]controllerId[RS]...
                                    "master" included means master self-flashes last
FW_TRANSFER_BEGIN_ACK: transfer-id<US>status
                      status = "OK" on success; otherwise a snake_case rejection
                      code (e.g., "sd_full", "busy", "unsupported_version")
FW_CHUNK:             transfer-id<US>seq<US>payload-len<US>base64-bytes<US>crc16-hex
FW_CHUNK_ACK:         transfer-id<US>highest-contiguous-seq<US>next-expected-seq<US>window-remaining
FW_CHUNK_NAK:         transfer-id<US>last-good-seq<US>reason-code   // CRC|SIZE|OUT_OF_ORDER|FLASH_FULL
FW_TRANSFER_END:      transfer-id<US>total-chunks<US>final-sha256-hex
FW_TRANSFER_END_ACK:  transfer-id<US>OK|HASH_MISMATCH|IO_ERROR<US>computed-sha256-hex
FW_DEPLOY_BEGIN:      transfer-id<US>order-list   // ordered ids, padawans first, master last
FW_PROGRESS:          transfer-id<US>controller-id<US>stage<US>bytes-sent<US>total-bytes<US>detail
                      stage value: see "Stage enum" above
FW_DEPLOY_DONE:       transfer-id<US>per-controller-result-list
                      result = controllerId<US>OK|FAILED<US>finalVersion<US>errorOrEmpty (RS-separated)
FW_BACKPRESSURE:      transfer-id<US>PAUSE|RESUME<US>reason
```

### Timing

- Per-frame ACK timeout: **1500 ms**, up to 3 retries.
- Whole-transfer watchdog: **5 minutes**.

### Happy path (Body + Core + Dome all selected)

```
Server                                Master
  |--FW_TRANSFER_BEGIN------------------>|     (open SD file)
  |<-FW_TRANSFER_BEGIN_ACK----------------|
  |--FW_CHUNK seq=0..N (sliding window)-->|     (write to SD, update streaming SHA)
  |<-FW_CHUNK_ACK ...---------------------|
  |--FW_TRANSFER_END--------------------->|     (verify SD hash)
  |<-FW_TRANSFER_END_ACK OK---------------|
  |--FW_DEPLOY_BEGIN order=core,dome,master->|
  |<-FW_PROGRESS core SENDING...----------|
  |<-FW_PROGRESS core VERIFYING-----------|
  |<-FW_PROGRESS core REBOOTING-----------|
  |<-FW_PROGRESS core VERSION_CONFIRMED---|     (master saw new version in heartbeat)
  |<-FW_PROGRESS dome ...same sequence----|
  |<-FW_PROGRESS master REBOOTING---------|     (master flashes self last)
  |<-FW_DEPLOY_DONE all=OK----------------|     (sent BEFORE master commits + reboots)
  |<-(silence ~8s while master reboots)---|
  |<-(POLL_ACK heartbeat: master version=target)
                                                Server holds the JobLock until this heartbeat.
```

### Failure modes

- CRC fail → `FW_CHUNK_NAK` with `last-good-seq` → server resumes from N+1.
- Hash mismatch at end → `FW_TRANSFER_END_ACK HASH_MISMATCH` → server retries the whole transfer once, then fails the job.
- SD full → `FW_TRANSFER_BEGIN_ACK` rejects → fail job fast.
- Padawan unreachable / reboot timeout → master moves to next padawan, marks the failed one in `FW_DEPLOY_DONE`. ("Continue past failures" decision.)

## B. Master ↔ Padawan (ESP-NOW)

Constraint: 250-byte ESP-NOW frame. Existing per-frame overhead leaves ≈150 bytes usable after CRC + seq fields. Frames are **binary packed structs**, not US/RS-delimited — every byte matters.

### Message types

| Name | Direction | Purpose |
|---|---|---|
| `OTA_BEGIN` | master → padawan | Announce: transfer-id, total size, SHA-256, chunk-size. |
| `OTA_BEGIN_ACK` / `OTA_BEGIN_NAK` | padawan → master | `esp_ota_begin` succeeded / no free OTA slot. |
| `OTA_DATA` | master → padawan | One chunk: transfer-id, seq, len, CRC-16, payload. |
| `OTA_DATA_ACK` / `OTA_DATA_NAK` | padawan → master | Per-frame ACK with `next-expected-seq`, `window-remaining`. |
| `OTA_END` | master → padawan | End-of-stream + final SHA-256. |
| `OTA_END_ACK` | padawan → master | `OK` / `HASH_MISMATCH` / `WRITE_ERROR` + computed hash. |
| `OTA_COMMIT` | master → padawan | `esp_ota_set_boot_partition` + reboot. |
| `OTA_COMMIT_ACK` | padawan → master | About to reboot. (No post-reboot message; heartbeat carries the new version.) |

### Frame layouts

Binary, fixed-offset, little-endian unless noted:

```
OTA_BEGIN payload (≈40 bytes):
  uint8  transfer-id
  uint32 total-size
  uint16 chunk-size            // recommend 128 bytes per OTA_DATA
  uint32 total-chunks
  uint8[32] sha256-expected
  uint8  flags                 // bit0=enable-psram-buffer; rest reserved

OTA_DATA payload (≈148 bytes):
  uint8  transfer-id
  uint32 seq
  uint16 payload-len
  uint16 crc16-ccitt           // poly 0x1021, init 0xFFFF; over [transfer-id..payload]
  uint8  payload[chunk-size]

OTA_DATA_ACK / OTA_DATA_NAK:
  uint8  transfer-id
  uint32 highest-contiguous-seq
  uint32 next-expected-seq
  uint8  window-remaining      // backpressure
  uint8  reason-code           // 0 on ACK; CRC|WRITE|OUT_OF_ORDER|HEAP on NAK

OTA_END payload:                { uint8 transfer-id; uint32 total-chunks-sent; uint8[32] sha256-final; }
OTA_END_ACK payload:            { uint8 transfer-id; uint8 status; uint8[32] sha256-computed; }
OTA_COMMIT payload:             { uint8 transfer-id; }
OTA_COMMIT_ACK payload:         { uint8 transfer-id; uint8 rebooting; }   // rebooting = 1
```

### Timing

- Per-frame ACK timeout: **400 ms**, up to 3 retries.
- `OTA_BEGIN_ACK` timeout: **2 s** (padawan may erase a partition).
- `OTA_END_ACK` timeout: **5 s** (full SHA-256 over 1.2 MB on ESP32 ≈ 1–3 s).
- Inter-padawan idle: **0 ms**.
- Post-`OTA_COMMIT` heartbeat poll: master polls padawan's heartbeat for `version === target` for **up to 15 s** before declaring `VERSION_CONFIRMED` or `FAILED detail=reboot_timeout`.

### Happy path (one padawan)

```
Master                                 Padawan
  |--OTA_BEGIN--------------------------->|   esp_ota_begin(inactive_part)
  |<-OTA_BEGIN_ACK------------------------|
  |--OTA_DATA seq=0..7------------------->|   esp_ota_write each
  |<-OTA_DATA_ACK next=8 win=8------------|
  |     ... (sliding window) ...          |
  |--OTA_END------------------------------>|   esp_ota_end + sha256 verify
  |<-OTA_END_ACK OK------------------------|
  |--OTA_COMMIT---------------------------->|   esp_ota_set_boot_partition
  |<-OTA_COMMIT_ACK rebooting=1-------------|
  |  (silent: padawan reboots)              |
  |<-(POLL_ACK heartbeat: version=target)---|
  master records VERSION_CONFIRMED, moves to next padawan
```

### Failure modes

- v1: abort the failing padawan and move on. Wire format leaves room for resume-from-seq-N (`next-expected-seq` field) but it is not implemented in v1.
- `OTA_END_ACK HASH_MISMATCH`: padawan refuses the commit; partition stays bootable on the previous image.
- Reboot watchdog: padawan boots into new firmware, must call `esp_ota_mark_app_valid_cancel_rollback` on first successful POLL_ACK back to master. Otherwise the bootloader auto-rolls back on next boot.

## How to amend this document

1. Edit the file in **both** repos in coordinated commits or PRs that reference each other.
2. When extending either enum, **append** new values — never reorder existing entries.
3. Note the cross-repo commit hash in the PR description so reviewers can verify the two copies match.
4. Sub-project plans that reference enum names from this file must be reviewed for compatibility.
