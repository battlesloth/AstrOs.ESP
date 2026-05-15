#include <AstrOsBulkTransport.hpp>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace
{
    // Helper: compute CRC over a string literal (excluding null terminator).
    uint16_t crc(const char *s)
    {
        return AstrOsBulkTransport::crc16_ccitt_false(reinterpret_cast<const uint8_t *>(s), std::strlen(s));
    }
} // namespace

//=================================================================================================
// CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection, no XOR-out)
//=================================================================================================

TEST(BulkTransport, Crc16EmptyInputReturnsInitValue)
{
    // Empty input: CRC equals the init value 0xFFFF.
    EXPECT_EQ(0xFFFFu, AstrOsBulkTransport::crc16_ccitt_false(nullptr, 0));
}

TEST(BulkTransport, Crc16CanonicalCheckVector)
{
    // "123456789" -> 0x29B1 per the canonical CCITT-FALSE test vector.
    EXPECT_EQ(0x29B1u, crc("123456789"));
}

TEST(BulkTransport, Crc16SingleZeroByte)
{
    // A single 0x00 byte: well-known CCITT-FALSE result 0xE1F0.
    const uint8_t data[] = {0x00};
    EXPECT_EQ(0xE1F0u, AstrOsBulkTransport::crc16_ccitt_false(data, 1));
}

TEST(BulkTransport, Crc16SingleFfByte)
{
    // A single 0xFF byte: well-known CCITT-FALSE result 0xFF00.
    const uint8_t data[] = {0xFF};
    EXPECT_EQ(0xFF00u, AstrOsBulkTransport::crc16_ccitt_false(data, 1));
}

TEST(BulkTransport, Crc16DeterministicAcrossCalls)
{
    // Same input on two separate invocations must produce the same output —
    // a regression guard for any future caching/state mistake.
    const uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF};
    uint16_t first = AstrOsBulkTransport::crc16_ccitt_false(data, 4);
    uint16_t second = AstrOsBulkTransport::crc16_ccitt_false(data, 4);
    EXPECT_EQ(first, second);
}

//=================================================================================================
// BulkReceiver::begin + reset + minimal onChunk happy path
//=================================================================================================

TEST(BulkTransport, BeginThenSingleChunkInOrderAcks)
{
    AstrOsBulkTransport::BulkReceiver r;
    r.begin(/*xferId=*/7, /*totalSize=*/12, /*totalChunks=*/1, /*chunkSize=*/12, /*windowSize=*/16);

    const uint8_t payload[] = {'H', 'e', 'l', 'l', 'o', ' ', 'W', 'o', 'r', 'l', 'd', '!'};
    uint16_t expectedCrc = AstrOsBulkTransport::crc16_ccitt_false(payload, sizeof(payload));

    auto result = r.onChunk(/*xferId=*/7, /*seq=*/0, sizeof(payload), expectedCrc, payload);

    EXPECT_EQ(AstrOsBulkTransport::Decision::ACK, result.decision);
    EXPECT_EQ(AstrOsBulkTransport::NakReason::NONE, result.reason);
    EXPECT_EQ(0u, result.highestContiguousSeq);
    EXPECT_EQ(1u, result.nextExpectedSeq);
    EXPECT_EQ(16u, result.windowRemaining); // matches the windowSize passed to begin()
    EXPECT_EQ(payload, result.payload);     // pointer passes through unmodified
    EXPECT_EQ(sizeof(payload), result.payloadLen);
}

TEST(BulkTransport, OnChunkBeforeBeginNaksOutOfOrder)
{
    AstrOsBulkTransport::BulkReceiver r;
    // No begin() called.
    const uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};
    uint16_t crc = AstrOsBulkTransport::crc16_ccitt_false(payload, 4);

    auto result = r.onChunk(/*xferId=*/7, /*seq=*/0, 4, crc, payload);

    EXPECT_EQ(AstrOsBulkTransport::Decision::NAK, result.decision);
    EXPECT_EQ(AstrOsBulkTransport::NakReason::OUT_OF_ORDER, result.reason);
}

TEST(BulkTransport, ResetReturnsToInactive)
{
    AstrOsBulkTransport::BulkReceiver r;
    r.begin(/*xferId=*/7, /*totalSize=*/100, /*totalChunks=*/1, /*chunkSize=*/100, /*windowSize=*/8);
    r.reset();

    // After reset, onChunk should behave the same as if begin() had never been called.
    const uint8_t payload[] = {0x01, 0x02};
    uint16_t crc = AstrOsBulkTransport::crc16_ccitt_false(payload, 2);
    auto result = r.onChunk(/*xferId=*/7, /*seq=*/0, 2, crc, payload);

    EXPECT_EQ(AstrOsBulkTransport::Decision::NAK, result.decision);
    EXPECT_EQ(AstrOsBulkTransport::NakReason::OUT_OF_ORDER, result.reason);
}

TEST(BulkTransport, BeginAfterEndReusesReceiver)
{
    AstrOsBulkTransport::BulkReceiver r;
    r.begin(/*xferId=*/7, /*totalSize=*/4, /*totalChunks=*/1, /*chunkSize=*/4, /*windowSize=*/16);

    const uint8_t firstPayload[] = {'a', 'b', 'c', 'd'};
    uint16_t firstCrc = AstrOsBulkTransport::crc16_ccitt_false(firstPayload, 4);
    auto first = r.onChunk(7, 0, 4, firstCrc, firstPayload);
    ASSERT_EQ(AstrOsBulkTransport::Decision::ACK, first.decision);

    // Reset, begin a new transfer with a different xferId, and accept its first chunk.
    r.reset();
    r.begin(/*xferId=*/9, /*totalSize=*/4, /*totalChunks=*/1, /*chunkSize=*/4, /*windowSize=*/16);

    const uint8_t secondPayload[] = {'e', 'f', 'g', 'h'};
    uint16_t secondCrc = AstrOsBulkTransport::crc16_ccitt_false(secondPayload, 4);
    auto second = r.onChunk(9, 0, 4, secondCrc, secondPayload);

    EXPECT_EQ(AstrOsBulkTransport::Decision::ACK, second.decision);
    EXPECT_EQ(0u, second.highestContiguousSeq);
    EXPECT_EQ(1u, second.nextExpectedSeq);
}
