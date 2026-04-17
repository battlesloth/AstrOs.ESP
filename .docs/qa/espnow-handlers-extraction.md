# QA — EspNow handler extraction (Phase 1)

Verifies that extracting the nine single-record ESP-NOW handlers into `lib_native/AstrOsEspNowProtocol` has not changed observable behaviour on the hardware side. The native test suite covers the decode shape; this plan covers the adapter wiring + interface-queue handoff + display updates.

## Preconditions

- Two metro_s3 boards flashed with the current branch (`feature/espnow-handlers-extraction`). One configured as master, one as padawan. Master connected via USB to the AstrOs.Server host running the web UI.
- Register the padawan with the master (send REGISTRATION_SYNC from the UI until both nodes' OLEDs show the peer in their list). This exercises the still-adapter-resident registration path and confirms Phase 2-deferred types are unaffected.

## Test cases

1. **Deploy config → padawan**
   - Action: from the UI, push a controller config to the padawan.
   - Expected: master serial monitor shows `SET_CONFIG` interface-response, padawan serial monitor shows "Config received" log, padawan OLED shows the config fingerprint. `ESP_LOGD Config ack/nak received` no longer appears — the pure lib returns `Ok` without emitting that debug line.
   - Also verify: send a malformed config (two parts instead of three) by manually injecting via the dev REPL if available. Master should log `Invalid config payload: …` via `ESP_LOGE` at `TAG=AstrOsEspNow`.

2. **Deploy script (multi-packet) → padawan**
   - Action: push a script that exceeds ~170 bytes so it fragments across 2+ ESP-NOW packets.
   - Expected: padawan OLED shows "script saved" on completion; `pio device monitor` on the padawan shows the pure lib's `Pending → Pending → Ok` sequence via the adapter's `Ok` log (or equivalent). Script runs when triggered.

3. **Run script → padawan**
   - Action: trigger the deployed script from the UI.
   - Expected: padawan executes the animation; master receives `SCRIPT_RUN_ACK` which is forwarded to the interface queue with `responseType=SCRIPT_RUN_ACK` (visible in the web UI as success).

4. **Panic stop → padawan**
   - Action: hit the UI's panic-stop button while a script is playing.
   - Expected: padawan halts the animation immediately. The three-part sentinel ("PANIC") on the wire is not logged or displayed — just the msgId is forwarded.

5. **Format SD → padawan**
   - Action: trigger a format-SD from the UI.
   - Expected: padawan formats its SD partition, OLED shows "SD formatted", master receives `FORMAT_SD_ACK`.

6. **Servo test (slider burst) → padawan**
   - Action: drag a slider in the UI for ~5 seconds so many SERVO_TEST packets flood the mesh.
   - Expected: servos track smoothly. No ACKs arrive back at the master (intentional; the padawan doesn't ACK servo-test to keep slider traffic cheap).

7. **Run command → padawan**
   - Action: send a run-command (any serial or GPIO command registered in the padawan's config) from the UI.
   - Expected: padawan executes it and returns `COMMAND_ACK`/`COMMAND_NAK`.

## Edge cases / negative tests

- **Padawan receives REGISTRATION_REQ or POLL_ACK** (wrong-role, master-only types): padawan should silently drop the packet without logging ERROR. Confirm by temporarily spoofing such a packet (e.g. with two padawans on the same channel and no master) and checking padawan logs show only a `ESP_LOGD Dropping packet type N destined for the other role` line.
- **Unknown packet type**: if the wire format drift ever lands a type outside the enum, master logs `Unknown packet type received` followed by the raw preview (same behaviour as pre-extraction).

## Regression shields

- `pio test -e test` — 175 test cases must pass (146 pre-Phase-1 + 29 new EspNow protocol cases).
- `pio run -e metro_s3` + `pio run -e lolin_d32_pro` — both builds clean with no new warnings.
- Registration and Poll flows (untouched this phase) are exercised implicitly by the preconditions: if they regressed, the peer wouldn't appear in the master's OLED list.
