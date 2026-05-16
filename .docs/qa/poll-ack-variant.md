# POLL_ACK — `variant` field QA

Verifies that POLL_ACK from both master and padawan paths carries the correct `variant` string end-to-end, and that legacy / Phase 1 padawans (no variant) gracefully relay as empty without poisoning the server's variant cache.

## Preconditions

- Bench rig: one master + at least one padawan, both reachable over ESP-NOW.
- Firmware on master built from this branch (`feature/ota-phase3-wire-up`) — `AstrOsConstants::Variant` is baked in at build time from `$PIOENV`.
- Padawan firmware: either same branch (reports variant) OR a legacy build (no variant) — both scenarios are exercised.
- AstrOs.Server running with a recent build that accepts 3, 4, or 5 fields in POLL_ACK (`message_handler.ts:90-118`). Note: 5-field POLL_ACK is c.6c.1+; older servers reject as `Invalid poll ack`.
- Serial monitor on master OR server log tail available so the raw POLL_ACK payload can be inspected.

## Test cases

### 1. Master self-POLL_ACK carries its build variant

1. Boot the master fresh.
2. Wait ~2 s for the first `pollingTimerCallback` tick (master's polling timer is 2 s periodic).
3. Tail the server log or watch the serial output; look for the POLL_ACK line carrying mac `00:00:00:00:00:00`.
4. **Pass:** the payload has 5 `<US>`-separated fields: `00:00:00:00:00:00 | master | <fingerprint> | <version> | <variant>` where `<variant>` matches the PlatformIO env the master was built with (`lolin_d32_pro` or `metro_s3`).
5. **Pass:** server's `controllerVariantCache` contains an entry for `00:00:00:00:00:00` with the reported variant. Confirm by attempting a firmware flash that targets the master only — the flash POST should NOT return `controllers_unknown` for the master.
6. **Fail:** payload has only 4 fields (no variant), OR the 5th field is empty, OR the variant doesn't match the build env.

### 2. Padawan POLL_ACK relays its build variant through master

1. Boot a padawan running this branch (variant-reporting build). Wait ~2 s for the master's next poll cycle.
2. The padawan responds over ESP-NOW with its 4-field payload (`name<US>fingerprint<US>version<US>variant`).
3. The master's `handlePollAck` parses, forwards to `main.cpp`'s `SEND_POLL_ACK` case, which emits the 5-field serial POLL_ACK to the server.
4. Tail the server log; find the POLL_ACK line carrying the padawan's MAC.
5. **Pass:** payload has 5 fields, 5th field matches the padawan's build env (e.g., `metro_s3` if the padawan is the new board).
6. **Pass:** server's variantCache populates for the padawan's MAC; a flash targeting that padawan no longer surfaces `controllers_unknown`.
7. **Fail:** 5th field is empty, OR matches the master's variant rather than the padawan's (would indicate the master is substituting its own variant instead of relaying the peer's).

### 3. Legacy padawan (no variant) relays as empty 5th field

1. With a padawan running a legacy build (pre-this-branch firmware that emits a 3-field ESP-NOW payload), wait for the master's poll cycle.
2. The padawan responds with 3 fields (`name<US>fingerprint<US>version`).
3. The master's `handlePollAck` reads `peerVariant = ""` (no 5th field present).
4. The interface-queue handoff packs as `fingerprint<US>version<US>` (trailing empty stripped by splitString in main.cpp).
5. `sendPollAckNak` is called with `variant=""`, `getPollAck` emits an explicit 5th `<US>` followed by nothing.
6. **Pass:** server-side splitString on the POLL_ACK payload yields 4 pieces (trailing empty stripped) — the legacy peer's POLL_ACK looks identical to the legacy 4-field shape on the wire.
7. **Pass:** server's variant cache stays empty for that padawan's MAC (guard at `api_server.ts:870`: `variant.length > 0` is false).
8. **Pass:** a flash targeting that padawan now returns `controllers_unknown` with the padawan's MAC in the detail string — the operator can see specifically WHICH controller needs a firmware update.
9. **Fail:** server logs `Invalid poll ack` (wrong-count rejection) for the legacy padawan, OR variant cache populates with an empty string for that MAC.

### 4. Bootstrapping a fresh fleet from cold

1. Power-cycle both master and at least one padawan, AND restart the server (wiping the in-memory `controllerVariantCache`).
2. Within ~5 s (one full master poll cycle), the master should send its self-POLL_ACK and relay at least one padawan POLL_ACK.
3. **Pass:** server's `controllerVariantCache` populates for both master and reachable padawans within the first poll cycle after server restart.
4. **Pass:** a firmware flash POST that selects either controller succeeds (no `controllers_unknown` error).
5. **Fail:** cache stays empty after multiple poll cycles despite firmware-side variant reporting being known-good (would indicate a regression in the serial parse path or the cache-population guard).

### 5. Adding a new board variant (regression smoke)

1. Add a new env to `platformio.ini` (e.g., `[env:test_board]`). Do NOT need to flash it — this is a build-time check.
2. Run `pio run -e test_board`.
3. **Pass:** `lib_native/AstrOsUtility/src/version_generated.hpp` after the build contains `constexpr const char *Variant = "test_board";`. Auto-derives from `$PIOENV`.
4. **Pass:** the binary's POLL_ACK self-report would carry `test_board` as the variant (verify by `strings .pio/build/test_board/firmware.elf | grep test_board`).
5. **Fail:** the variant constant is empty or hardcoded to a different env name.

## Notes

- The PlatformIO env name doubles as the firmware-asset variant on the server. Keep them in sync: if the server's release-asset naming convention changes from `astros-esp-<v>-<variant>-app.bin` to anything that doesn't match the PIO env names, this contract breaks silently. Tracking the asset-naming convention as cross-repo documentation would be a separate follow-up.
- Legacy peer relays produce an explicit empty 5th field at the firmware-out wire, which splitString-strips into the legacy 4-field shape at the server. This is deliberate — the empty variant should be invisible to the server, indistinguishable from a peer that never reported one.
- The reboot-watchdog + post-flash heartbeat flow (`pollResponseTimeExpired`, `decidePostDeployHeartbeat`) does not depend on variant. Variant is purely an OTA-asset-selection input.
