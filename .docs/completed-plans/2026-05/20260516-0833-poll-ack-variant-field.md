# POLL_ACK — emit `variant` field

## Problem

The c.6c.1 protocol design extended POLL_ACK with a 5th field `variant` so the server can pick the right firmware asset (`astros-esp-<version>-<variant>-app.bin`) at flash time. The **server-side** parser was wired for it (`message_handler.ts:104-115` accepts 4 OR 5 fields) and the server's `controllerVariantCache` is keyed by MAC → variant. **The firmware side was never finished** — `getPollAck()` in `lib_native/AstrOsMessaging` still builds a 4-field payload (`mac<US>name<US>fingerprint<US>version`), no variant. Grep confirms: the literal word `variant` does not appear anywhere in `lib/`, `lib_native/`, or `src/` (only matches in unrelated README prose).

Surfacing now because the in-server firmware-flash flow (currently being tested on the AstrOs.Server `feature/firmware-allow-downgrade` branch) gates on `controllerVariantCache` being populated, which it can't be until firmware emits variant. The server's new `controllers_unknown` error correctly flags this end-to-end ("the MACs you selected aren't in my variant cache") but there's no path to populate the cache until this firmware-side work lands.

## Approach

Bake the variant string into the firmware binary at build time via `scripts/version_gen.py` — the same generator that emits `Version` and `GitSha` into `version_generated.hpp`. The variant comes from the PlatformIO env name (`env.subst("$PIOENV")` resolves to `lolin_d32_pro` / `metro_s3` / `test`). Auto-derives from env, so adding a new board doesn't require a separate variant decl.

Thread `AstrOsConstants::Variant` through the POLL_ACK paths:

- **Padawan → master ESP-NOW POLL_ACK** (`AstrOsEspNowService.cpp:681-682`): currently 3 fields (`name<US>fingerprint<US>version`). Add variant as a 4th. The master unpacks and forwards.
- **Master interface-queue handoff to main.cpp** (`AstrOsEspNowService.cpp:732-737`): currently packs `fingerprint<US>version`. Add variant as a 3rd packed piece.
- **Main.cpp SEND_POLL_ACK dispatch** (`main.cpp:836-846`): unpack fingerprint + version + variant; pass to `sendPollAckNak`.
- **Serial-out POLL_ACK** (`getPollAck`, `sendPollAckNak`): take an additional `variant` arg; emit as the 5th `<US>`-separated field on the wire.

Master's own POLL_ACK (sent to itself / the server via the interface queue) uses `AstrOsConstants::Variant` directly. Padawan-relayed POLL_ACKs use the unpacked peer variant.

## Tasks

- [x] **Variant constant.** Extend `scripts/version_gen.py` to read `env.subst("$PIOENV")` and emit `constexpr const char *Variant = "<env>";` into `version_generated.hpp`. The CLI fallback path (no PIO context) emits `Variant = "native"` so standalone runs of the script don't error. Verify the generated header still compiles unchanged when run from both PIO and CLI contexts.

- [x] **ESP-NOW POLL_ACK wire format (padawan side).** Update the padawan's POLL_ACK send in `AstrOsEspNowService.cpp:680-685` to append `<US>variant` (4 fields total). Variant source is `AstrOsConstants::Variant`. Update the master's `handlePollAck` (`AstrOsEspNowService.cpp:699-740`) to parse the 4th field, fall back to empty string for legacy peers (protocol-amendment "append, never reorder" rule).

- [x] **Interface queue handoff.** Master's `handlePollAck` packs `fingerprint<US>version<US>variant` into the queue message (was `fingerprint<US>version`). `main.cpp:836-846` splits into 3 pieces and threads variant through to `sendPollAckNak`. Legacy peers (no variant) yield empty `variant` — preserved through the chain.

- [x] **Serial-out POLL_ACK (5th field).** `AstrOsSerialMessageService::getPollAck` gains a `variant` parameter, appends `<US>variant` to the existing 4-field payload. `AstrOsSerialMsgHandler::sendPollAckNak` gains a matching parameter. Master's self-POLL_ACK callsite (in `main.cpp` — the master's own polling response) passes `AstrOsConstants::Variant`. Padawan-relayed callsite (the SEND_POLL_ACK case in `main.cpp:836`) passes the unpacked peer variant.

- [x] **Native tests + QA plan.** Update `test/test_native/astros_serial_messages_tests.cpp` POLL_ACK assertions: pin the new 5-field format for getPollAck. Add a case for an empty `variant` argument (legacy-peer path) — should emit 5 fields with the last empty so the server's `String.trim() === ''` guard still skips the cache update. QA plan in `.docs/qa/poll-ack-variant.md`: master self-POLL_ACK carries its build variant; padawan POLL_ACK is relayed with the padawan's variant; legacy padawan relays as empty variant.

## Files touched

- `scripts/version_gen.py`
- `lib_native/AstrOsUtility/src/AstrOsConstants.hpp` (comment update only; `Variant` lives in the generated header)
- `lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.hpp`
- `lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.cpp`
- `lib/AstrOsSerialMsgHandler/include/AstrOsSerialMsgHandler.hpp`
- `lib/AstrOsSerialMsgHandler/src/AstrOsSerialMsgHandler.cpp`
- `lib/AstrOsEspNow/src/AstrOsEspNowService.cpp`
- `src/main.cpp` (SEND_POLL_ACK case + the master's own self-POLL_ACK send path)
- `test/test_native/astros_serial_messages_tests.cpp`
- `.docs/qa/poll-ack-variant.md` (new)

## Out of scope

- **Server-side coordination commit.** The AstrOs.Server's `message_handler.ts:104-115` already parses 4 OR 5 fields, and the variantCache write skips empty variants. No server change needed; the cross-repo "append, never reorder" rule is preserved here.
- **Persistent variant storage** (NVS-backed or DB-backed). The build-time constant is sufficient — a controller's variant doesn't change without a re-flash anyway, and a re-flash regenerates `version_generated.hpp`.
- **Backwards compat for existing padawans on legacy firmware.** Their POLL_ACK relays will produce empty `variant` at the server, which keeps them out of the variantCache (server already handles this case). Operators can flash them via USB to bootstrap.
- **Protocol doc rewrite.** `.docs/protocol.md` only references POLL_ACK in OTA flow context, not the wire-format level. Adding a wire-format section is a larger doc task; tracking as a follow-up. (If you'd like, I can squeeze a one-line wire-format note into the doc as part of task 4.)
