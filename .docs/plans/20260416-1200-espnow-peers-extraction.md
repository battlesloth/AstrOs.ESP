# AstrOsEspNow Extraction — Phase 2: PeerList

## Context

Phase 1 (committed on this same branch, `feature/espnow-handlers-extraction`) extracted the nine single-record packet handlers into `lib_native/AstrOsEspNowProtocol`. Registration and poll paths stayed in the MIXED adapter because they mutate the `peers` vector — state that has no pure home today.

Phase 2 extracts that state into a pure `PeerList` class in a new sibling lib, `lib_native/AstrOsEspNowPeers`. The registration + poll FSMs themselves stay in the adapter for now; they call `PeerList` through the existing `peersMutex`. This closes the code-review P0-9 finding (unprotected `peers` vector accesses) by making the vector unreachable except via `PeerList` methods — the mutex contract becomes syntactic rather than by-convention.

Phase 1 + Phase 2 ship in a single PR (stacked commits on this branch). OTA, landing next on top of ESP-NOW, will need to track firmware upload progress per peer; a well-bounded `PeerList` is the right place for that field to land when it's needed.

Intended outcome after Phase 2:

- `peers` vector is private to `PeerList`; every read and write in the adapter flows through the lock.
- `pollAckThisCycle` lifecycle (reset → marked by ack → evaluated on expire) is named and testable via `resetPollCycle` / `markPollAckReceived` / `listUnacked`.
- Dead `espnow_peer_t::id` field is removed.
- Native tests cover add / dedup / capacity / poll-cycle semantics — invariants that previously lived in comments and call-site inspection.

## Scope decisions (locked in)

| Question | Decision |
|---|---|
| Lib name | `AstrOsEspNowPeers` (sibling to `AstrOsEspNowProtocol`) |
| API shape | Class `PeerList` with intent-revealing methods |
| Synchronisation | **No locks inside `PeerList`.** Adapter keeps `peersMutex` and calls `take → PeerList op → give`. Matches the `AnimationController`/`AstrOsAnimationEngine` pattern. |
| Peer struct | Relocated from `AstrOsEspNowUtility.h` to the new pure header, renamed `Peer`, stripped of the dead `id` field. `ESP_NOW_ETH_ALEN` becomes a local `ETH_MAC_LEN = 6` constant. |
| MAC comparison | By `std::string` (the canonical `"AA:BB:..."` form from `AstrOsStringUtils::macToString`). Byte overload added only if a caller needs it. |
| Poll semantics | Split the existing `isValidPollPeer` (lookup + mutate) into `contains` (read) and `markPollAckReceived` (write). |
| Scope exclusions | Registration FSM extraction, poll FSM extraction, spin-wait getter cleanup — all deferred to Phase 3. |
| Branching | Stacks on `feature/espnow-handlers-extraction`. Phase 1 + Phase 2 merge together as one PR. |

## Public API

```cpp
// lib_native/AstrOsEspNowPeers/include/AstrOsEspNowPeers.hpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace AstrOsEspNowPeers
{
    constexpr std::size_t ETH_MAC_LEN = 6;
    constexpr std::size_t PEER_LIMIT = 10;
    constexpr std::size_t PEER_NAME_MAX = 16;

    struct Peer
    {
        uint8_t macAddr[ETH_MAC_LEN] = {};
        char name[PEER_NAME_MAX] = {};
        char cryptoKey[PEER_NAME_MAX] = {};
        bool isPaired = false;
        bool pollAckThisCycle = false;
    };

    enum class AddResult { Added, AlreadyExists, Full };

    class PeerList
    {
    public:
        AddResult add(const Peer &peer);

        bool contains(const std::string &macString) const;
        std::optional<Peer> findByMac(const std::string &macString) const;

        // Poll cycle: reset flags, mark ack on one peer, list peers still unacked.
        void resetPollCycle();
        bool markPollAckReceived(const std::string &macString);
        std::vector<Peer> listUnacked() const;

        // Snapshot (for display / getPeers()). Returns a copy so callers can
        // release the adapter's mutex before touching the network.
        std::vector<Peer> all() const;
        std::size_t size() const;
        bool isFull() const;
        void clear();

    private:
        std::vector<Peer> peers_;
    };
}
```

No logging inside the lib; errors surface as `AddResult` values or `std::optional` absence. Caller logs diagnostics if relevant.

## Task checklist (5 tasks, one commit each)

- [ ] **Task 1 — Scaffold the pure lib + relocate `Peer`.** Create `lib_native/AstrOsEspNowPeers/{include,src}` with the header above, stub `.cpp`, and a `README` (purity rule, forbidden-include list). Append `lib_native/AstrOsEspNowPeers` to `PURE_LIBS` in `.github/workflows/pr-validation.yml`. Leave `espnow_peer_t` in `AstrOsEspNowUtility.h` alone for now — it'll be removed in Task 4. *Verify:* `pio run -e test` builds; purity guard passes locally.
- [ ] **Task 2 — Implement `PeerList`.** Flesh out all ten methods. Keep bodies small; the class is mostly `std::vector` operations with a MAC comparison helper. *Verify:* native test file created in Task 3 compiles against this.
- [ ] **Task 3 — Native tests.** New file `test/test_native/astros_espnow_peers_tests.cpp`. ~10 cases: add success / dedup / capacity, contains / findByMac hit+miss, pollCycle reset, markPollAckReceived hit+miss, listUnacked snapshot, all snapshot, clear. *Verify:* `pio test -e test` goes from 175 → ~185 passing cases.
- [ ] **Task 4 — Rewrite adapter to use `PeerList`.** In `lib/AstrOsEspNow/src/AstrOsEspNowService.cpp`: replace `std::vector<espnow_peer_t> peers` with `AstrOsEspNowPeers::PeerList peers`. Update `init`, `cachePeer`, `findPeer`, `getPeers`, `pollPadawans`, `pollRepsonseTimeExpired`. Split `isValidPollPeer` at its two callsites (one read, one write). Remove `espnow_peer_t` from `AstrOsEspNowUtility.h` and `ESPNOW_PEER_LIMIT` if no references remain. Update `astros_espnow_config_t` to take `PeerList` references or raw `Peer[]` — whichever keeps the `init()` call site in `main.cpp` simplest. *Verify:* both firmware builds clean; all existing tests still pass.
- [ ] **Task 5 — Docs + QA update.** Update `CLAUDE.md` Library-layout table with the new PURE lib row. Extend the existing `.docs/qa/espnow-handlers-extraction.md` (same file Phase 1 wrote) with a new "Phase 2 — PeerList" section that adds the two-padawan invariant: both register, drop one's power, master emits NAK after one poll cycle, surviving peer still appears in `getPeers()`. *Verify:* docs rendered, QA file committed alongside the code.

## Critical files

**Created:**

- `lib_native/AstrOsEspNowPeers/include/AstrOsEspNowPeers.hpp`
- `lib_native/AstrOsEspNowPeers/src/AstrOsEspNowPeers.cpp`
- `lib_native/AstrOsEspNowPeers/README`
- `test/test_native/astros_espnow_peers_tests.cpp`

**Modified:**

- `lib/AstrOsEspNow/src/AstrOsEspNowService.cpp` — replace peers vector + vector ops with `PeerList`
- `lib/AstrOsEspNow/src/AstrOsEspNowService.hpp` — type change on the `peers` member; remove `isValidPollPeer` declaration if fully inlined away
- `lib/AstrOsEspNow/src/AstrOsEspNowUtility.h` — delete `espnow_peer_t` typedef; keep ESP-IDF constants
- `src/main.cpp` — adapt `astros_espnow_config_t.peers` / `peerCount` init path if the config struct changes
- `.github/workflows/pr-validation.yml` — append lib to `PURE_LIBS`
- `CLAUDE.md` — Library-layout table row
- `.docs/qa/espnow-handlers-extraction.md` — add two-padawan poll cycle case

## Existing code to reuse

| Utility | Location | Role |
|---|---|---|
| `AstrOsStringUtils::macToString(uint8_t*)` | `lib_native/AstrOsUtility/src/AstrOsStringUtils.hpp` | Build the canonical `"AA:BB:..."` form used by `contains` / `findByMac` / `markPollAckReceived` |
| `espnow_peer_t` layout | `lib/AstrOsEspNow/src/AstrOsEspNowUtility.h:49-57` | Template for the relocated `Peer` struct (drop `id`) |
| `cachePeer` body | `lib/AstrOsEspNow/src/AstrOsEspNowService.cpp:222-268` | Current implementation — the add + dedup + capacity logic ports almost verbatim into `PeerList::add` |
| `findPeer` / `isValidPollPeer` bodies | `lib/AstrOsEspNow/src/AstrOsEspNowService.cpp:1185-1230` | Current lookup + mutate logic — splits cleanly into `contains` + `markPollAckReceived` |
| `pollPadawans` / `pollRepsonseTimeExpired` | `lib/AstrOsEspNow/src/AstrOsEspNowService.cpp:630-671, 732-762` | Shape the `resetPollCycle` / `listUnacked` method design |

## Verification

- **Native tests:** `pio test -e test` — all existing suites still pass; new `astros_espnow_peers_tests.cpp` contributes ~10 cases.
- **Firmware builds:** `pio run -e metro_s3` and `pio run -e lolin_d32_pro` — clean.
- **CI purity guard:** appended `lib_native/AstrOsEspNowPeers`; the new lib's files must not contain `freertos/`, `esp_`, `driver/`, `nvs_`, `sdmmc_` includes.
- **Hardware QA:** registration + poll flows covered alongside Phase 1 (`.docs/qa/espnow-handlers-extraction.md`). New case: two padawans register; drop one's power; master emits NAK after one poll cycle; the surviving peer still appears in `getPeers()`.

## What's explicitly out of scope (Phase 3 candidates)

- Registration FSM extraction (`handleRegistrationReq/Registration/RegistrationAck` → pure state-transition functions).
- Poll cycle FSM extraction (`pollPadawans`, `handlePollAck`, `pollRepsonseTimeExpired`) with an injectable time source.
- Spin-wait getter cleanup (code-review P1-16 — `getMac`, `getName`, `getFingerprint`).
- OTA-specific fields on `Peer` (e.g., `otaBytesReceived`) — land those with the OTA feature, not here.
