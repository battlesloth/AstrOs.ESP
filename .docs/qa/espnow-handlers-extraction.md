# QA — EspNow handler + PeerList extraction (Phases 1 & 2)

Verifies that extracting the nine single-record ESP-NOW handlers into `lib_native/AstrOsEspNowProtocol` (Phase 1) and the peer-list storage into `lib_native/AstrOsEspNowPeers` (Phase 2) has not changed observable behaviour on the hardware side. The native test suite covers the pure shapes; this plan covers the adapter wiring + interface-queue handoff + display updates + registration and poll cycle flows.

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

## Phase 2 — PeerList cases

All of the above flows exercise the PeerList indirectly (every ESP-NOW send/receive reaches it via the adapter). The new cases below target the peer-list invariants specifically.

8. **Fresh registration populates PeerList + persists across reboot**
   - Action: cold-boot an unregistered padawan against a registered master. Send REGISTRATION_SYNC from the UI.
   - Expected: padawan appears in master's `getPeers()` output (OLED name list), assigned name follows the existing roster order (Ashoka → Grogu → Anakin → Obi-Wan). Reboot the master, confirm the padawan is reloaded from NVS — on-disk layout is unchanged (the extraction preserves `espnow_peer_t` byte-for-byte).

9. **Duplicate registration does not double-insert**
   - Action: with a padawan already registered, send REGISTRATION_SYNC a second time.
   - Expected: master OLED peer count does not increase; logs show `Peer already cached` (the `AddResult::AlreadyExists` path).

10. **Two padawans register; one drops → master emits SEND_POLL_NAK after one cycle**
    - Action: register two padawans with the master. After both appear in the peer list, power off one. Wait up to 4 s (one full 2 s poll + 2 s expiry).
    - Expected: master emits `SEND_POLL_NAK` to the interface queue for the dropped padawan, OLED shows it as offline; the surviving padawan still shows online and its `SEND_POLL_ACK` lands every cycle.

11. **Poll cycle flag resets cleanly — surviving peer stays online across cycles**
    - Action: after case 10, power the dropped padawan back on. Wait for the next poll cycle.
    - Expected: the revived padawan's next `POLL_ACK` flips its flag via `markPollAckReceived`, and the next `pollRepsonseTimeExpired` no longer emits a NAK for it. (If `resetPollCycle` weren't wiring both slots correctly the peer could get stuck in a NAK loop — this case catches that regression.)

## Edge cases / negative tests

- **Padawan receives REGISTRATION_REQ or POLL_ACK** (wrong-role, master-only types): padawan should silently drop the packet without logging ERROR. Confirm by temporarily spoofing such a packet (e.g. with two padawans on the same channel and no master) and checking padawan logs show only a `ESP_LOGD Dropping packet type N destined for the other role` line.
- **Unknown packet type**: if the wire format drift ever lands a type outside the enum, master logs `Unknown packet type received` followed by the raw preview (same behaviour as pre-extraction).
- **Peer cache at capacity** (10 peers): attempt to register an 11th. Master should log `Peer cache is full` and reject the registration without crashing. Restart clears the cached peers (unless persisted) — verify.

## Regression shields

- `pio test -e test` — 185 test cases must pass (146 pre-Phase-1 + 29 Phase 1 + 10 Phase 2 cases).
- `pio run -e metro_s3` + `pio run -e lolin_d32_pro` — both builds clean with no new warnings.
- NVS peer-config on-disk layout is unchanged across the Phase 2 refactor. An existing device upgraded to this branch must reload its cached peers without re-registration.
