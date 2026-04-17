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
        AddResult add(const Peer &peer);

        bool contains(const std::string &macString) const;
        std::optional<Peer> findByMac(const std::string &macString) const;

        // Start of a new poll cycle: clears every peer's pollAckThisCycle flag.
        void resetPollCycle();

        // Records that `macString` responded to the current poll. Returns
        // false if the MAC is not in the list (no mutation performed).
        bool markPollAckReceived(const std::string &macString);

        // Peers whose pollAckThisCycle flag is still false. Returns copies
        // so the caller can release the mutex before acting on the list.
        std::vector<Peer> listUnacked() const;

        // Full snapshot for display / adapter::getPeers(). Returns a copy.
        std::vector<Peer> all() const;

        std::size_t size() const;
        bool isFull() const;
        void clear();

    private:
        std::vector<Peer> peers_;
    };

} // namespace AstrOsEspNowPeers
