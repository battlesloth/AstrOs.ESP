#include <AstrOsEspNowPeers.hpp>

#include <AstrOsStringUtils.hpp>

#include <cstring>

namespace AstrOsEspNowPeers
{
    namespace
    {
        bool macEquals(const uint8_t (&macAddr)[ESPNOW_ETH_MAC_LEN], const std::string &macString)
        {
            return AstrOsStringUtils::macToString(macAddr) == macString;
        }
    } // namespace

    AddResult PeerList::add(const espnow_peer_t &peer)
    {
        // Check for duplicates BEFORE the capacity guard so idempotent
        // re-registration keeps working in a full mesh — a padawan
        // re-sending REGISTRATION_SYNC after all PEER_LIMIT slots are
        // already taken should hit the AlreadyExists path, not Full.
        for (const auto &existing : peers_)
        {
            if (std::memcmp(existing.mac_addr, peer.mac_addr, ESPNOW_ETH_MAC_LEN) == 0)
            {
                return AddResult::AlreadyExists;
            }
        }

        if (isFull())
        {
            return AddResult::Full;
        }

        peers_.push_back(peer);
        return AddResult::Added;
    }

    bool PeerList::contains(const std::string &macString) const
    {
        return findByMac(macString).has_value();
    }

    std::optional<espnow_peer_t> PeerList::findByMac(const std::string &macString) const
    {
        for (const auto &p : peers_)
        {
            if (macEquals(p.mac_addr, macString))
            {
                return p;
            }
        }
        return std::nullopt;
    }

    void PeerList::resetPollCycle()
    {
        for (auto &p : peers_)
        {
            p.pollAckThisCycle = false;
        }
    }

    bool PeerList::markPollAckReceived(const std::string &macString)
    {
        for (auto &p : peers_)
        {
            if (macEquals(p.mac_addr, macString))
            {
                p.pollAckThisCycle = true;
                return true;
            }
        }
        return false;
    }

    std::vector<espnow_peer_t> PeerList::listUnacked() const
    {
        std::vector<espnow_peer_t> out;
        for (const auto &p : peers_)
        {
            if (!p.pollAckThisCycle)
            {
                out.push_back(p);
            }
        }
        return out;
    }

    std::vector<espnow_peer_t> PeerList::all() const
    {
        return peers_;
    }

    std::size_t PeerList::size() const
    {
        return peers_.size();
    }

    bool PeerList::isFull() const
    {
        return peers_.size() >= ESPNOW_PEER_LIMIT;
    }

    void PeerList::clear()
    {
        peers_.clear();
    }

} // namespace AstrOsEspNowPeers
