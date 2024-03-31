#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <AstrOsMessaging.hpp>
#include <PacketTracker.hpp>

TEST(PacketTracker, AddPacket)
{
    auto tracker = PacketTracker();

    PacketData data;

    data.packetNumber = 1;
    data.totalPackets = 2;
    data.payload = "test";

    auto result = tracker.addPacket("test", data, 1000);

    EXPECT_EQ(AddPacketResult::SUCCESS, result);
}

TEST(PacketTracker, GetMessageOnePacket)
{
    auto tracker = PacketTracker();

    PacketData data;

    data.packetNumber = 1;
    data.totalPackets = 2;
    data.payload = "test";

    auto result = tracker.addPacket("abcd", data, 1000);

    EXPECT_EQ(AddPacketResult::SUCCESS, result);

    auto message = tracker.getMessage("abcd");

    EXPECT_STREQ("test", message.c_str());

    auto message2 = tracker.getMessage("abcd");

    EXPECT_STREQ("", message2.c_str());
}

TEST(PacketTracker, GetMessage)
{
    auto tracker = PacketTracker();

    PacketData data;

    data.packetNumber = 1;
    data.totalPackets = 2;
    data.payload = "test";

    auto result = tracker.addPacket("abcd", data, 1000);

    EXPECT_EQ(AddPacketResult::SUCCESS, result);

    data.packetNumber = 2;
    data.totalPackets = 2;
    data.payload = "test2";

    result = tracker.addPacket("abcd", data, 1200);

    EXPECT_EQ(AddPacketResult::MESSAGE_COMPLETE, result);

    auto message = tracker.getMessage("abcd");

    EXPECT_STREQ("testtest2", message.c_str());
}

TEST(PacketTracker, ExpireMessages)
{
    auto tracker = PacketTracker();

    PacketData data;

    data.packetNumber = 1;
    data.totalPackets = 2;
    data.payload = "test";

    auto result = tracker.addPacket("test", data, 1000);

    data.packetNumber = 2;
    data.totalPackets = 2;
    data.payload = "test2";

    result = tracker.addPacket("test2", data, 3000);

    auto message = tracker.getMessage("test");

    EXPECT_STREQ("", message.c_str());
}

TEST(PacketTracker, PacketExists)
{

    auto tracker = PacketTracker();

    PacketData data;

    data.packetNumber = 1;
    data.totalPackets = 2;
    data.payload = "test";

    auto result = tracker.addPacket("test", data, 1000);

    data.packetNumber = 1;
    data.totalPackets = 2;
    data.payload = "test2";

    result = tracker.addPacket("test", data, 1200);

    EXPECT_EQ(AddPacketResult::PACKET_EXISTS, result);
}

TEST(PacketTracker, MessageExpiredTimerOverflow)
{
    auto tracker = PacketTracker();

    PacketData data;

    data.packetNumber = 1;
    data.totalPackets = 2;
    data.payload = "test";

    auto result = tracker.addPacket("ABCD", data, 1500);

    EXPECT_EQ(AddPacketResult::SUCCESS, result);

    data.packetNumber = 2;
    data.totalPackets = 2;
    data.payload = "test";

    result = tracker.addPacket("ABCD", data, 1600);

    EXPECT_EQ(AddPacketResult::MESSAGE_COMPLETE, result);

    data.packetNumber = 1;
    data.totalPackets = 2;
    data.payload = "test2";

    result = tracker.addPacket("WXYZ", data, 1200);

    auto message = tracker.getMessage("test");

    EXPECT_STREQ("", message.c_str());
}
