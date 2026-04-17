#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <espnow_peer.h>

namespace AstrOsEspNowPeers
{
    enum class AddResult
    {
        Added,
        AlreadyExists,
        Full,
    };

    // Bounded container for registered ESP-NOW peers. All MAC lookups use
    // the canonical uppercase "AA:BB:CC:DD:EE:FF" string form produced by
    // AstrOsStringUtils::macToString(). The class holds no synchronisation;
    // callers protect it with their own mutex.
    class PeerList
    {
    public:
        AddResult add(const espnow_peer_t &peer);

        bool contains(const std::string &macString) const;
        std::optional<espnow_peer_t> findByMac(const std::string &macString) const;

        // Start of a new poll cycle: clears every peer's pollAckThisCycle flag.
        void resetPollCycle();

        // Records that `macString` responded to the current poll. Returns
        // false if the MAC is not in the list (no mutation performed).
        bool markPollAckReceived(const std::string &macString);

        // Peers whose pollAckThisCycle flag is still false. Returns copies
        // so the caller can release the mutex before acting on the list.
        std::vector<espnow_peer_t> listUnacked() const;

        // Full snapshot for display / adapter::getPeers(). Returns a copy.
        std::vector<espnow_peer_t> all() const;

        std::size_t size() const;
        bool isFull() const;
        void clear();

    private:
        std::vector<espnow_peer_t> peers_;
    };

} // namespace AstrOsEspNowPeers
