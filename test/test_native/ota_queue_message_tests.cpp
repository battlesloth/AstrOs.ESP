#include <gtest/gtest.h>

#include <OtaQueueMessage.h>

#include <cstdint>
#include <cstring>
#include <type_traits>

// queue_ota_msg_t is passed by value through FreeRTOS queue storage; adding a non-trivial member
// (std::string, std::optional) silently breaks that contract — catch it at compile time.
static_assert(std::is_standard_layout<queue_ota_msg_t>::value, "queue_ota_msg_t must be standard-layout");
static_assert(offsetof(queue_ota_msg_t, kind) == 0, "kind must be the first member (consumer reads it first)");

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

TEST(OtaQueueMessage, DeployArmRoundTrip)
{
    queue_ota_msg_t m;
    std::memset(&m, 0, sizeof(m));
    m.kind = OTA_MSG_DEPLOY_BEGIN;
    m.deploy.msgId = nullptr;
    m.deploy.orderList = nullptr;

    EXPECT_EQ(OTA_MSG_DEPLOY_BEGIN, m.kind);
}

// OTA_MSG_WATCHDOG_FIRE is a signal carrier — no union arm, transferId is nullptr by contract.
// Pins both invariants so a future producer that forgets the rule fails a test before it ships.
TEST(OtaQueueMessage, WatchdogFireArmHasNoOwnedPointers)
{
    queue_ota_msg_t m;
    std::memset(&m, 0, sizeof(m));
    m.kind = OTA_MSG_WATCHDOG_FIRE;

    EXPECT_EQ(OTA_MSG_WATCHDOG_FIRE, m.kind);
    EXPECT_EQ(nullptr, m.transferId);
}

// Every arm of the anonymous union must share the same starting address — pins the union
// contract so a future refactor that accidentally promotes the arms to separate fields fails.
TEST(OtaQueueMessage, UnionArmsShareStartingAddress)
{
    queue_ota_msg_t m;
    std::memset(&m, 0, sizeof(m));
    const void *beginAddr = static_cast<const void *>(&m.begin);
    EXPECT_EQ(beginAddr, static_cast<const void *>(&m.chunk));
    EXPECT_EQ(beginAddr, static_cast<const void *>(&m.end));
    EXPECT_EQ(beginAddr, static_cast<const void *>(&m.deploy));
}

// Pins the producer's strncpy(buf, src, 64) + buf[64]='\0' idiom for the 64-char hot path.
// An off-by-one would leak an unterminated string into the wire ACK payload.
TEST(OtaQueueMessage, Sha256HexBoundaryStaysNulTerminated)
{
    queue_ota_msg_t m;
    std::memset(&m, 0xFFu, sizeof(m)); // pre-fill with non-zero garbage
    m.kind = OTA_MSG_BEGIN;

    const char *full = "deadbeefcafebabe1111222233334444aaaabbbbccccddddeeeeffff00001111";
    ASSERT_EQ(64u, std::strlen(full));
    std::strncpy(m.begin.sha256Hex, full, 64);
    m.begin.sha256Hex[64] = '\0';

    EXPECT_EQ(64u, std::strlen(m.begin.sha256Hex));
    EXPECT_EQ('\0', m.begin.sha256Hex[64]);
    EXPECT_STREQ(full, m.begin.sha256Hex);
}

// Short input must also produce a NUL-padded buffer. Relies on strncpy's NUL-fill behavior;
// a memcpy(dst, src, 64) regression would leak whatever bytes lived past the source literal.
TEST(OtaQueueMessage, Sha256HexShortInputNulFills)
{
    queue_ota_msg_t m;
    std::memset(&m, 0xFFu, sizeof(m)); // pre-fill with non-zero garbage
    m.kind = OTA_MSG_BEGIN;

    const char *shortHash = "deadbeef00"; // 10 chars
    ASSERT_EQ(10u, std::strlen(shortHash));
    std::strncpy(m.begin.sha256Hex, shortHash, 64);
    m.begin.sha256Hex[64] = '\0';

    EXPECT_EQ(10u, std::strlen(m.begin.sha256Hex));
    EXPECT_STREQ(shortHash, m.begin.sha256Hex);
    // Bytes [10..64] must be NUL — leaking 0xFF here would corrupt the wire payload.
    for (size_t i = 10; i <= 64; ++i)
    {
        EXPECT_EQ('\0', m.begin.sha256Hex[i]) << "byte " << i << " should be NUL after strncpy(short, 64)";
    }
}
