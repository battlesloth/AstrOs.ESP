#include <AstrOsEspNowPeers.hpp>

#include <AstrOsStringUtils.hpp>

#include <algorithm>
#include <cstring>

namespace AstrOsEspNowPeers
{
    namespace
    {
        bool macEquals(const uint8_t (&macAddr)[ETH_MAC_LEN], const std::string &macString)
        {
            return AstrOsStringUtils::macToString(macAddr) == macString;
        }
    } // namespace

    AddResult PeerList::add(const Peer &peer)
    {
        if (isFull())
        {
            return AddResult::Full;
        }

        for (const auto &existing : peers_)
        {
            if (std::memcmp(existing.macAddr, peer.macAddr, ETH_MAC_LEN) == 0)
            {
                return AddResult::AlreadyExists;
            }
        }

        peers_.push_back(peer);
        return AddResult::Added;
    }

    bool PeerList::contains(const std::string &macString) const
    {
        return findByMac(macString).has_value();
    }

    std::optional<Peer> PeerList::findByMac(const std::string &macString) const
    {
        for (const auto &p : peers_)
        {
            if (macEquals(p.macAddr, macString))
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
            if (macEquals(p.macAddr, macString))
            {
                p.pollAckThisCycle = true;
                return true;
            }
        }
        return false;
    }

    std::vector<Peer> PeerList::listUnacked() const
    {
        std::vector<Peer> out;
        for (const auto &p : peers_)
        {
            if (!p.pollAckThisCycle)
            {
                out.push_back(p);
            }
        }
        return out;
    }

    std::vector<Peer> PeerList::all() const
    {
        return peers_;
    }

    std::size_t PeerList::size() const
    {
        return peers_.size();
    }

    bool PeerList::isFull() const
    {
        return peers_.size() >= PEER_LIMIT;
    }

    void PeerList::clear()
    {
        peers_.clear();
    }

} // namespace AstrOsEspNowPeers
