#include <AstrOsEspNowPeers.hpp>

namespace AstrOsEspNowPeers
{
    AddResult PeerList::add(const Peer & /*peer*/)
    {
        return AddResult::Full;
    }

    bool PeerList::contains(const std::string & /*macString*/) const
    {
        return false;
    }

    std::optional<Peer> PeerList::findByMac(const std::string & /*macString*/) const
    {
        return std::nullopt;
    }

    void PeerList::resetPollCycle() {}

    bool PeerList::markPollAckReceived(const std::string & /*macString*/)
    {
        return false;
    }

    std::vector<Peer> PeerList::listUnacked() const
    {
        return {};
    }

    std::vector<Peer> PeerList::all() const
    {
        return {};
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
