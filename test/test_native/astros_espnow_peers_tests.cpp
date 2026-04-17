#include <AstrOsEspNowPeers.hpp>
#include <AstrOsStringUtils.hpp>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>

namespace
{
    using AstrOsEspNowPeers::AddResult;
    using AstrOsEspNowPeers::ETH_MAC_LEN;
    using AstrOsEspNowPeers::Peer;
    using AstrOsEspNowPeers::PEER_LIMIT;
    using AstrOsEspNowPeers::PeerList;

    // Builds a peer with MAC AA:BB:CC:DD:EE:{lastOctet:02X}.
    Peer makePeer(uint8_t lastOctet, const char *name = "pad")
    {
        Peer p{};
        p.macAddr[0] = 0xAA;
        p.macAddr[1] = 0xBB;
        p.macAddr[2] = 0xCC;
        p.macAddr[3] = 0xDD;
        p.macAddr[4] = 0xEE;
        p.macAddr[5] = lastOctet;
        std::strncpy(p.name, name, sizeof(p.name) - 1);
        p.isPaired = true;
        return p;
    }

    std::string macOf(uint8_t lastOctet)
    {
        Peer p = makePeer(lastOctet);
        return AstrOsStringUtils::macToString(p.macAddr);
    }
} // namespace

// ---------------- add ----------------

TEST(PeerList, AddReturnsAddedOnEmptyList)
{
    PeerList list;
    EXPECT_EQ(AddResult::Added, list.add(makePeer(0x01)));
    EXPECT_EQ(1u, list.size());
}

TEST(PeerList, AddDuplicateMacReturnsAlreadyExistsAndDoesNotGrow)
{
    PeerList list;
    EXPECT_EQ(AddResult::Added, list.add(makePeer(0x01, "first")));
    EXPECT_EQ(AddResult::AlreadyExists, list.add(makePeer(0x01, "duplicate")));
    EXPECT_EQ(1u, list.size());

    // First insertion wins; the duplicate attempt does not overwrite the name.
    auto fetched = list.findByMac(macOf(0x01));
    ASSERT_TRUE(fetched.has_value());
    EXPECT_STREQ("first", fetched->name);
}

TEST(PeerList, AddReturnsFullAtCapacity)
{
    PeerList list;
    for (std::size_t i = 0; i < PEER_LIMIT; ++i)
    {
        ASSERT_EQ(AddResult::Added, list.add(makePeer(static_cast<uint8_t>(i + 1))));
    }
    EXPECT_TRUE(list.isFull());
    EXPECT_EQ(AddResult::Full, list.add(makePeer(0xFF)));
    EXPECT_EQ(PEER_LIMIT, list.size());
}

// ---------------- contains / findByMac ----------------

TEST(PeerList, ContainsHitAndMiss)
{
    PeerList list;
    list.add(makePeer(0x01));

    EXPECT_TRUE(list.contains(macOf(0x01)));
    EXPECT_FALSE(list.contains(macOf(0x02)));
}

TEST(PeerList, FindByMacReturnsStoredPeer)
{
    PeerList list;
    list.add(makePeer(0x01, "alpha"));
    list.add(makePeer(0x02, "beta"));

    auto found = list.findByMac(macOf(0x02));
    ASSERT_TRUE(found.has_value());
    EXPECT_STREQ("beta", found->name);

    EXPECT_FALSE(list.findByMac(macOf(0x03)).has_value());
}

// ---------------- poll cycle ----------------

TEST(PeerList, ResetPollCycleClearsAllFlags)
{
    PeerList list;
    list.add(makePeer(0x01));
    list.add(makePeer(0x02));
    list.markPollAckReceived(macOf(0x01));
    list.markPollAckReceived(macOf(0x02));

    list.resetPollCycle();

    EXPECT_EQ(2u, list.listUnacked().size());
}

TEST(PeerList, MarkPollAckReceivedHitAndMiss)
{
    PeerList list;
    list.add(makePeer(0x01));
    list.add(makePeer(0x02));

    EXPECT_TRUE(list.markPollAckReceived(macOf(0x01)));
    EXPECT_FALSE(list.markPollAckReceived(macOf(0x03)));

    auto unacked = list.listUnacked();
    ASSERT_EQ(1u, unacked.size());
    EXPECT_EQ(0x02, unacked[0].macAddr[5]);
}

TEST(PeerList, ListUnackedAfterSelectiveAcks)
{
    PeerList list;
    list.add(makePeer(0x01, "one"));
    list.add(makePeer(0x02, "two"));
    list.add(makePeer(0x03, "three"));

    list.resetPollCycle();
    list.markPollAckReceived(macOf(0x02));

    auto unacked = list.listUnacked();
    ASSERT_EQ(2u, unacked.size());
    EXPECT_STREQ("one", unacked[0].name);
    EXPECT_STREQ("three", unacked[1].name);
}

// ---------------- snapshot / lifecycle ----------------

TEST(PeerList, AllReturnsSnapshotThatDoesNotAliasInternalState)
{
    PeerList list;
    list.add(makePeer(0x01, "before"));

    auto snapshot = list.all();
    ASSERT_EQ(1u, snapshot.size());
    std::strncpy(snapshot[0].name, "mutated", sizeof(snapshot[0].name) - 1);

    auto stored = list.findByMac(macOf(0x01));
    ASSERT_TRUE(stored.has_value());
    EXPECT_STREQ("before", stored->name);
}

TEST(PeerList, ClearEmptiesTheList)
{
    PeerList list;
    list.add(makePeer(0x01));
    list.add(makePeer(0x02));

    list.clear();

    EXPECT_EQ(0u, list.size());
    EXPECT_FALSE(list.isFull());
    EXPECT_FALSE(list.contains(macOf(0x01)));
}
