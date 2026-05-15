#include <gtest/gtest.h>

#include <OtaQueueMessage.h>

#include <cstdint>
#include <cstring>

TEST(OtaQueueMessage, BeginArmRoundTrip)
{
    queue_ota_msg_t m;
    std::memset(&m, 0, sizeof(m));
    m.kind = OTA_MSG_BEGIN;
    m.transferId = nullptr;
    m.begin.msgId = nullptr;
    m.begin.totalSize = 1234567;
    m.begin.totalChunks = 300;
    m.begin.chunkSize = 4096;
    std::strncpy(m.begin.sha256Hex, "deadbeefcafebabe1111222233334444aaaabbbbccccddddeeeeffff00001111", 64);
    m.begin.sha256Hex[64] = '\0';
    m.begin.targetList = nullptr;

    EXPECT_EQ(OTA_MSG_BEGIN, m.kind);
    EXPECT_EQ(1234567u, m.begin.totalSize);
    EXPECT_EQ(300u, m.begin.totalChunks);
    EXPECT_EQ(4096u, m.begin.chunkSize);
    EXPECT_STREQ("deadbeefcafebabe1111222233334444aaaabbbbccccddddeeeeffff00001111", m.begin.sha256Hex);
}

TEST(OtaQueueMessage, ChunkArmRoundTrip)
{
    queue_ota_msg_t m;
    std::memset(&m, 0, sizeof(m));
    m.kind = OTA_MSG_CHUNK;
    m.transferId = nullptr;
    m.chunk.seq = 17;
    m.chunk.payloadLen = 4096;
    m.chunk.crc16 = 0xABCD;
    m.chunk.payload = nullptr;

    EXPECT_EQ(OTA_MSG_CHUNK, m.kind);
    EXPECT_EQ(17u, m.chunk.seq);
    EXPECT_EQ(4096u, m.chunk.payloadLen);
    EXPECT_EQ(0xABCDu, m.chunk.crc16);
}

TEST(OtaQueueMessage, EndArmRoundTrip)
{
    queue_ota_msg_t m;
    std::memset(&m, 0, sizeof(m));
    m.kind = OTA_MSG_END;
    m.end.msgId = nullptr;
    m.end.totalChunks = 300;
    std::strncpy(m.end.finalSha256Hex, "00000000000000000000000000000000ffffffffffffffffffffffffffffffff", 64);
    m.end.finalSha256Hex[64] = '\0';

    EXPECT_EQ(OTA_MSG_END, m.kind);
    EXPECT_EQ(300u, m.end.totalChunks);
}

TEST(OtaQueueMessage, KindDiscriminatesUnion)
{
    queue_ota_msg_t m;
    std::memset(&m, 0, sizeof(m));
    m.kind = OTA_MSG_DEPLOY_BEGIN;
    m.deploy.msgId = nullptr;
    m.deploy.orderList = nullptr;

    EXPECT_EQ(OTA_MSG_DEPLOY_BEGIN, m.kind);
}
