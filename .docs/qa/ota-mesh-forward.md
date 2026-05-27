# OTA Mesh-Forward QA Plan

Covers bench validation for PR set 1 (M1-M5) of the firmware OTA mesh-forward
work. Each milestone adds its own sub-section. M5 will extend with 2-padawan
and negative-path cases.

## Preconditions

- AstrOs.Server `develop` branch running, Firmware view loaded in browser
- At least one master node + one padawan node, paired via the existing
  ESP-NOW peer registration flow (master polling shows padawan responding to
  POLL every 2 s before starting any test)
- Firmware artifact for testing: any build output, e.g.
  `.pio/build/metro_s3/firmware.bin` (~1.2 MB)
- Padawan USB cable accessible (for post-test partition dumps via esptool.py)

## M3 — master OtaForwarder emits frames (no padawan handler yet)

This case was validated informally during M3 bench checkpoint; recorded here
for completeness.

1. Trigger `FW_DEPLOY_BEGIN` from server UI with order list `[pad1]`
2. Expected: master serial log shows `OTA_BEGIN` emission attempt to pad1
3. Expected: after ~2 s, master log shows `forwarder_done: pad1=FAILED reason="begin_ack_timeout"` (padawan returned no ACK because OtaWriter was stubbed)
4. Expected: server UI shows `pad1: FAILED` with the begin-ack-timeout reason

## M4 — single-padawan end-to-end

The M4 merge-bar test. **This case must pass for M4 PR to merge.**

### Setup
- 1 master + 1 padawan on bench
- Master has SD card with firmware staging directory writable
- Padawan has SD card (per [[project_all_nodes_have_sd]] — every node has SD)
- Both nodes flashed with the M4-candidate firmware

### Test case 1: happy path

1. Upload `firmware.bin` (≥ 1 MB, ≤ partition size) via server Firmware view
2. Confirm master serial log shows `OtaReceiver: handleEnd: success — renamed to /sdcard/firmware/<sha>.bin`
3. Click "Deploy" in server UI with order list `[pad1]`
4. Expected master log sequence:
   - `forwarder: start xferId=1 padawan=pad1`
   - `forwarder: emitted OTA_BEGIN xferId=1 to <pad1-mac>`
   - `forwarder: BEGIN_ACK received xferId=1 from <pad1-mac>`
   - Many `forwarder: streaming chunk seq=N` (or DEBUG-only)
   - `forwarder: END_ACK received xferId=1 status=OK`
   - `forwarder_done: pad1=OK`
5. Expected padawan log sequence:
   - `OtaWriter: handleBegin accepted: xferId=1 totalSize=N chunks=K chunkSize=128 partition='ota_X' (size=M, offset=0x...)`
   - Many `OtaWriter: handleData ... accepted` (DEBUG only) — no NAKs
   - `OtaWriter: handleEnd: transfer xferId=1 OK — N bytes verified on partition 'ota_X'`
6. Expected server UI: `pad1: OK`, deploy summary shows 1/1 successful

### Test case 2: byte-level verification

After test case 1 succeeds:

1. Disconnect padawan from mesh (or simply note its USB serial port)
2. Connect padawan via USB
3. Dump the inactive partition:
   ```bash
   # 8 MB board (ota_1 starts at 0x220000, size 0x200000):
   esptool.py --port /dev/ttyUSB1 read_flash 0x220000 0x200000 padawan_dump.bin
   # 16 MB board (ota_1 starts at 0x650000, size 0x640000):
   esptool.py --port /dev/ttyUSB1 read_flash 0x650000 0x640000 padawan_dump.bin
   ```
   Adjust offset based on which partition the padawan log identified as inactive.
4. Truncate to actual firmware size + compare SHA:
   ```bash
   FW_SIZE=$(stat -c%s .pio/build/metro_s3/firmware.bin)
   dd if=padawan_dump.bin of=padawan_image.bin bs=1 count=$FW_SIZE 2>/dev/null
   sha256sum padawan_image.bin .pio/build/metro_s3/firmware.bin
   ```
   Both lines MUST produce identical sha. If they differ, M4 has a silent corruption bug — investigate before merging.

### Test case 3: padawan-side BUSY rejection

Validates that a duplicate BEGIN during an active transfer is rejected gracefully (vs corrupting state).

1. Start a deploy with order list `[pad1]`
2. While the chunk stream is in flight (within ~5 s of BEGIN), manually trigger a second `FW_DEPLOY_BEGIN` from the server (open a second browser tab; click Deploy again)
3. Expected padawan log: `handleBegin: xferId=2 arrived while xferId=1 is active — replying BUSY`
4. Expected master log: forwarder of second deploy logs `OTA_BEGIN_NAK reason=BUSY` and records pad1 as FAILED for that deploy
5. The first deploy continues to completion normally (pad1=OK for deploy #1)

### Test case 4: idle-watchdog recovery

Validates that a stuck transfer self-aborts within 10 s.

1. Modify master temporarily (do not commit): comment out the line in `OtaForwarder::streamDrain` that calls `bulk_.nextChunkToSend()`, so the master stops sending OTA_DATA after BEGIN_ACK
2. Rebuild + flash master only (`pio run -e metro_s3 -t upload`)
3. Trigger a deploy with order list `[pad1]`
4. Padawan receives BEGIN, ACKs, awaits chunks
5. Expected after ~10 s on padawan: `handleWatchdogFire: idle threshold (10000ms) exceeded for xferId=N — aborting transfer`
6. **Late-END silent drop**: without rebooting the padawan, re-enable `bulk_.nextChunkToSend()` on master (revert the test modification), then trigger another deploy with the SAME order list. Master's OtaForwarder may emit an OTA_END from the prior aborted attempt before the new BEGIN propagates. Expected padawan log on that stray END: `handleEnd: xferId=N arrived while inactive — silent drop (master END_ACK timeout will recover)` — confirms the cleanup #1 fix #3 silent-drop path. (If the stray END doesn't reproduce, skip — this sub-step is opportunistic.)
7. Confirm padawan `isActive()` returns false: trigger a fresh deploy; it must succeed (test case 1 happy path replay)
8. Revert any remaining master-side modifications

### Stack high-water mark check

After test case 1, add a temporary `ESP_LOGI(TAG, "ota_writer hwm=%d", uxTaskGetStackHighWaterMark(NULL));` at the bottom of `handleEnd`'s success branch. Re-flash padawan, re-run test case 1, record the value:

- Recorded value: _____ bytes remaining (target: ≥ 1 KB; expected ~3.5 KB with the 8 KB stack and 4 KB readback buffer)
- If < 1 KB: bump `otaWriterTask` stack further in main.cpp (currently 8192 after the T7 followup), re-test
- If comfortably > 2 KB: consider tuning back down toward 6144 to free RAM
- Remove the temporary log before merging

## M5 (placeholder)

To be filled in by M5's PR. Will cover:
- 2-padawan sequential deploy (both OK)
- 2-padawan deploy with one offline (master times out gracefully, marks FAILED, moves to next)
- 2-padawan deploy with mid-transfer power-cycle on one padawan (idle watchdog fires, padawan recovers cleanly on next BEGIN)
- FW_PROGRESS UI animation per padawan
