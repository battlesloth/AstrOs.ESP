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
    ASSERT_TRUE(r.begin(/*xferId=*/7, /*totalSize=*/12, /*totalChunks=*/1, /*chunkSize=*/12, /*windowSize=*/16).valid);

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
    // Not-active sentinel: windowRemaining == 0, no payload pass-through.
    EXPECT_EQ(0u, result.windowRemaining);
    EXPECT_EQ(nullptr, result.payload);
    EXPECT_EQ(0u, result.payloadLen);
}

TEST(BulkTransport, ResetReturnsToInactive)
{
    AstrOsBulkTransport::BulkReceiver r;
    ASSERT_TRUE(r.begin(/*xferId=*/7, /*totalSize=*/100, /*totalChunks=*/1, /*chunkSize=*/100, /*windowSize=*/8).valid);
    r.reset();

    const uint8_t payload[] = {0x01, 0x02};
    uint16_t crc = AstrOsBulkTransport::crc16_ccitt_false(payload, 2);
    auto result = r.onChunk(/*xferId=*/7, /*seq=*/0, 2, crc, payload);

    EXPECT_EQ(AstrOsBulkTransport::Decision::NAK, result.decision);
    EXPECT_EQ(AstrOsBulkTransport::NakReason::OUT_OF_ORDER, result.reason);
    EXPECT_EQ(0u, result.windowRemaining);
    EXPECT_EQ(nullptr, result.payload);
}

TEST(BulkTransport, BeginAfterEndReusesReceiver)
{
    AstrOsBulkTransport::BulkReceiver r;
    ASSERT_TRUE(r.begin(/*xferId=*/7, /*totalSize=*/4, /*totalChunks=*/1, /*chunkSize=*/4, /*windowSize=*/16).valid);

    const uint8_t firstPayload[] = {'a', 'b', 'c', 'd'};
    uint16_t firstCrc = AstrOsBulkTransport::crc16_ccitt_false(firstPayload, 4);
    auto first = r.onChunk(7, 0, 4, firstCrc, firstPayload);
    ASSERT_EQ(AstrOsBulkTransport::Decision::ACK, first.decision);

    // Reset, begin a new transfer with a different xferId, and accept its first chunk.
    r.reset();
    ASSERT_TRUE(r.begin(/*xferId=*/9, /*totalSize=*/4, /*totalChunks=*/1, /*chunkSize=*/4, /*windowSize=*/16).valid);

    const uint8_t secondPayload[] = {'e', 'f', 'g', 'h'};
    uint16_t secondCrc = AstrOsBulkTransport::crc16_ccitt_false(secondPayload, 4);
    auto second = r.onChunk(9, 0, 4, secondCrc, secondPayload);

    EXPECT_EQ(AstrOsBulkTransport::Decision::ACK, second.decision);
    EXPECT_EQ(0u, second.highestContiguousSeq);
    EXPECT_EQ(1u, second.nextExpectedSeq);
}

TEST(BulkTransport, BeginOnActiveReceiverReinitializes)
{
    // The contract says begin() on an already-active receiver reinitializes
    // it as if reset() + begin() had been called.
    AstrOsBulkTransport::BulkReceiver r;
    ASSERT_TRUE(r.begin(/*xferId=*/7, /*totalSize=*/8, /*totalChunks=*/2, /*chunkSize=*/4, /*windowSize=*/16).valid);

    const uint8_t payload[] = {'a', 'b', 'c', 'd'};
    uint16_t crc = AstrOsBulkTransport::crc16_ccitt_false(payload, 4);
    ASSERT_EQ(AstrOsBulkTransport::Decision::ACK, r.onChunk(7, 0, 4, crc, payload).decision);

    // Call begin() again with a different xferId mid-transfer, without
    // an intervening reset(). nextSeq_ should be zeroed back to 0.
    ASSERT_TRUE(r.begin(/*xferId=*/9, /*totalSize=*/4, /*totalChunks=*/1, /*chunkSize=*/4, /*windowSize=*/16).valid);

    auto result = r.onChunk(/*xferId=*/9, /*seq=*/0, 4, crc, payload);
    EXPECT_EQ(AstrOsBulkTransport::Decision::ACK, result.decision);
    EXPECT_EQ(0u, result.highestContiguousSeq);
    EXPECT_EQ(1u, result.nextExpectedSeq);
}

TEST(BulkTransport, BeginWithZeroChunkSizeLeavesReceiverInactive)
{
    AstrOsBulkTransport::BulkReceiver r;
    auto br = r.begin(/*xferId=*/7, /*totalSize=*/100, /*totalChunks=*/1, /*chunkSize=*/0, /*windowSize=*/16);

    // BeginResult surfaces the specific rejection reason so Phase 3 can
    // emit a matching FW_TRANSFER_BEGIN_ACK status code rather than
    // silently NAK every subsequent chunk.
    EXPECT_FALSE(br.valid);
    EXPECT_EQ(AstrOsBulkTransport::BeginResult::Reason::ZERO_CHUNK_SIZE, br.reason);

    // Receiver should be inactive — onChunk reports the not-active sentinel.
    const uint8_t payload[] = {0x01};
    uint16_t crc = AstrOsBulkTransport::crc16_ccitt_false(payload, 1);
    auto result = r.onChunk(7, 0, 1, crc, payload);

    EXPECT_EQ(AstrOsBulkTransport::Decision::NAK, result.decision);
    EXPECT_EQ(AstrOsBulkTransport::NakReason::OUT_OF_ORDER, result.reason);
    EXPECT_EQ(0u, result.windowRemaining); // not-active sentinel
}

TEST(BulkTransport, BeginWithZeroWindowSizeLeavesReceiverInactive)
{
    // Zero windowSize would defeat the windowRemaining=0 not-active
    // sentinel: every reply from an active receiver would also report
    // windowRemaining=0 (the configured size). begin() must reject so
    // Phase 3's "abort + restart" dispatch isn't permanently armed.
    AstrOsBulkTransport::BulkReceiver r;
    auto br = r.begin(/*xferId=*/7, /*totalSize=*/4, /*totalChunks=*/1, /*chunkSize=*/4, /*windowSize=*/0);

    EXPECT_FALSE(br.valid);
    EXPECT_EQ(AstrOsBulkTransport::BeginResult::Reason::ZERO_WINDOW_SIZE, br.reason);
}

TEST(BulkTransport, BeginWithTotalSizeAboveChunkGridLeavesReceiverInactive)
{
    // totalChunks=2, chunkSize=4 -> at most 8 bytes can fit. totalSize=9
    // means the sender claims more data than the chunk grid can hold:
    // structurally impossible. Reject before the SIZE math can be confused.
    AstrOsBulkTransport::BulkReceiver r;
    auto br = r.begin(/*xferId=*/7, /*totalSize=*/9, /*totalChunks=*/2, /*chunkSize=*/4, /*windowSize=*/16);

    EXPECT_FALSE(br.valid);
    EXPECT_EQ(AstrOsBulkTransport::BeginResult::Reason::SIZE_INCONSISTENT, br.reason);
}

TEST(BulkTransport, BeginWithTotalSizeBelowChunkGridLeavesReceiverInactive)
{
    // totalChunks=2, chunkSize=4 -> minimum valid totalSize is 5 (one
    // full chunk + at least one byte in the second). totalSize=4 means
    // the second chunk would have to be zero bytes, which the wire
    // protocol disallows.
    AstrOsBulkTransport::BulkReceiver r;
    auto br = r.begin(/*xferId=*/7, /*totalSize=*/4, /*totalChunks=*/2, /*chunkSize=*/4, /*windowSize=*/16);

    EXPECT_FALSE(br.valid);
    EXPECT_EQ(AstrOsBulkTransport::BeginResult::Reason::SIZE_INCONSISTENT, br.reason);
}

TEST(BulkTransport, BeginWithZeroTotalSizeLeavesReceiverInactive)
{
    // A zero-byte image is structurally meaningless — caught by the
    // explicit `totalSize == 0` arm of the consistency check.
    AstrOsBulkTransport::BulkReceiver r;
    auto br = r.begin(/*xferId=*/7, /*totalSize=*/0, /*totalChunks=*/1, /*chunkSize=*/4, /*windowSize=*/16);

    EXPECT_FALSE(br.valid);
    EXPECT_EQ(AstrOsBulkTransport::BeginResult::Reason::SIZE_INCONSISTENT, br.reason);
}

TEST(BulkTransport, BeginWithZeroTotalChunksLeavesReceiverInactive)
{
    // Note: the zero-totalChunks check runs BEFORE the zero-windowSize and
    // SIZE_INCONSISTENT checks (consistent with the order in begin()), so
    // a degenerate-on-multiple-axes BEGIN reports ZERO_TOTAL_CHUNKS — the
    // first structural problem the receiver hits.
    AstrOsBulkTransport::BulkReceiver r;
    auto br = r.begin(/*xferId=*/7, /*totalSize=*/0, /*totalChunks=*/0, /*chunkSize=*/4, /*windowSize=*/16);

    EXPECT_FALSE(br.valid);
    EXPECT_EQ(AstrOsBulkTransport::BeginResult::Reason::ZERO_TOTAL_CHUNKS, br.reason);

    const uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};
    uint16_t crc = AstrOsBulkTransport::crc16_ccitt_false(payload, 4);
    auto result = r.onChunk(7, 0, 4, crc, payload);

    EXPECT_EQ(AstrOsBulkTransport::Decision::NAK, result.decision);
    EXPECT_EQ(0u, result.windowRemaining);

    // onEnd on the inactive receiver should not return OK, and should
    // surface NOT_ACTIVE as the distinct IO_ERROR reason.
    auto end = r.onEnd(7, 0);
    EXPECT_EQ(AstrOsBulkTransport::EndResult::Status::IO_ERROR, end.status);
    EXPECT_EQ(AstrOsBulkTransport::EndResult::Reason::NOT_ACTIVE, end.reason);
}

//=================================================================================================
// BulkReceiver::onChunk — CRC + SIZE rejection
//=================================================================================================

TEST(BulkTransport, OnChunkBadCrcNaks)
{
    AstrOsBulkTransport::BulkReceiver r;
    ASSERT_TRUE(r.begin(/*xferId=*/7, /*totalSize=*/4, /*totalChunks=*/1, /*chunkSize=*/4, /*windowSize=*/16).valid);

    const uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};
    uint16_t realCrc = AstrOsBulkTransport::crc16_ccitt_false(payload, 4);
    auto result = r.onChunk(7, 0, 4, /*crc16=*/static_cast<uint16_t>(realCrc ^ 0xFFFFu), payload);

    EXPECT_EQ(AstrOsBulkTransport::Decision::NAK, result.decision);
    EXPECT_EQ(AstrOsBulkTransport::NakReason::CRC, result.reason);
    // No chunk committed yet -> reported as 0/0.
    EXPECT_EQ(0u, result.highestContiguousSeq);
    EXPECT_EQ(0u, result.nextExpectedSeq);
    EXPECT_EQ(16u, result.windowRemaining);
    EXPECT_EQ(nullptr, result.payload);
    EXPECT_EQ(0u, result.payloadLen);
}

TEST(BulkTransport, OnChunkPayloadLenMismatchNaksSize)
{
    AstrOsBulkTransport::BulkReceiver r;
    // totalSize = 4096, chunkSize = 1024 -> 4 chunks of 1024 bytes each.
    ASSERT_TRUE(
        r.begin(/*xferId=*/7, /*totalSize=*/4096, /*totalChunks=*/4, /*chunkSize=*/1024, /*windowSize=*/16).valid);

    // Sender claims this is chunk 0 with len=500 — wrong; should be 1024.
    std::vector<uint8_t> payload(500, 0xAA);
    uint16_t crc = AstrOsBulkTransport::crc16_ccitt_false(payload.data(), payload.size());
    auto result = r.onChunk(7, 0, static_cast<uint16_t>(payload.size()), crc, payload.data());

    EXPECT_EQ(AstrOsBulkTransport::Decision::NAK, result.decision);
    EXPECT_EQ(AstrOsBulkTransport::NakReason::SIZE, result.reason);
    // First-chunk NAK: highestContiguousSeq=0, nextExpectedSeq=0 (sentinel).
    EXPECT_EQ(0u, result.highestContiguousSeq);
    EXPECT_EQ(0u, result.nextExpectedSeq);
    EXPECT_EQ(16u, result.windowRemaining);
    EXPECT_EQ(nullptr, result.payload);
    EXPECT_EQ(0u, result.payloadLen);
}

TEST(BulkTransport, OnChunkLastShortChunkAcks)
{
    AstrOsBulkTransport::BulkReceiver r;
    // totalSize = 2500, chunkSize = 1024 -> chunks of 1024, 1024, 452.
    ASSERT_TRUE(
        r.begin(/*xferId=*/7, /*totalSize=*/2500, /*totalChunks=*/3, /*chunkSize=*/1024, /*windowSize=*/16).valid);

    // Commit chunks 0 and 1.
    std::vector<uint8_t> fullChunk(1024, 0xCC);
    uint16_t fullCrc = AstrOsBulkTransport::crc16_ccitt_false(fullChunk.data(), fullChunk.size());
    auto r0 = r.onChunk(7, 0, 1024, fullCrc, fullChunk.data());
    ASSERT_EQ(AstrOsBulkTransport::Decision::ACK, r0.decision);
    auto r1 = r.onChunk(7, 1, 1024, fullCrc, fullChunk.data());
    ASSERT_EQ(AstrOsBulkTransport::Decision::ACK, r1.decision);

    // Last chunk: 452 bytes — the short tail.
    std::vector<uint8_t> tail(452, 0xDD);
    uint16_t tailCrc = AstrOsBulkTransport::crc16_ccitt_false(tail.data(), tail.size());
    auto r2 = r.onChunk(7, 2, 452, tailCrc, tail.data());
    EXPECT_EQ(AstrOsBulkTransport::Decision::ACK, r2.decision);
    EXPECT_EQ(2u, r2.highestContiguousSeq);
    EXPECT_EQ(3u, r2.nextExpectedSeq);
}

TEST(BulkTransport, OnChunkWrongPayloadLenOnLastChunkNaksSize)
{
    AstrOsBulkTransport::BulkReceiver r;
    // totalSize = 2500, chunkSize = 1024 -> last chunk expected = 452 bytes.
    ASSERT_TRUE(
        r.begin(/*xferId=*/7, /*totalSize=*/2500, /*totalChunks=*/3, /*chunkSize=*/1024, /*windowSize=*/16).valid);

    std::vector<uint8_t> fullChunk(1024, 0xCC);
    uint16_t fullCrc = AstrOsBulkTransport::crc16_ccitt_false(fullChunk.data(), fullChunk.size());
    ASSERT_EQ(AstrOsBulkTransport::Decision::ACK, r.onChunk(7, 0, 1024, fullCrc, fullChunk.data()).decision);
    ASSERT_EQ(AstrOsBulkTransport::Decision::ACK, r.onChunk(7, 1, 1024, fullCrc, fullChunk.data()).decision);

    // Last chunk: sender sends 1024 bytes but only 452 are expected.
    std::vector<uint8_t> wrongTail(1024, 0xDD);
    uint16_t crc = AstrOsBulkTransport::crc16_ccitt_false(wrongTail.data(), wrongTail.size());
    auto result = r.onChunk(7, 2, 1024, crc, wrongTail.data());

    EXPECT_EQ(AstrOsBulkTransport::Decision::NAK, result.decision);
    EXPECT_EQ(AstrOsBulkTransport::NakReason::SIZE, result.reason);
    // Mid-stream NAK after committing seq 0 and 1: highestContiguousSeq=1,
    // nextExpectedSeq=2, full window. Verifies the SIZE-NAK sync fields
    // when nextSeq_ > 0.
    EXPECT_EQ(1u, result.highestContiguousSeq);
    EXPECT_EQ(2u, result.nextExpectedSeq);
    EXPECT_EQ(16u, result.windowRemaining);
    EXPECT_EQ(nullptr, result.payload);
}

//=================================================================================================
// BulkReceiver::onChunk — duplicate, skip-forward + wrong-xferId rejection
//=================================================================================================

TEST(BulkTransport, OnChunkDuplicateSeqNaksOutOfOrder)
{
    AstrOsBulkTransport::BulkReceiver r;
    ASSERT_TRUE(r.begin(/*xferId=*/7, /*totalSize=*/8, /*totalChunks=*/2, /*chunkSize=*/4, /*windowSize=*/16).valid);

    const uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};
    uint16_t crc = AstrOsBulkTransport::crc16_ccitt_false(payload, 4);

    // ACK seq=0 cleanly.
    auto first = r.onChunk(7, 0, 4, crc, payload);
    ASSERT_EQ(AstrOsBulkTransport::Decision::ACK, first.decision);
    EXPECT_EQ(1u, first.nextExpectedSeq);

    // Sender retransmits seq=0 — receiver has already moved on.
    auto duplicate = r.onChunk(7, 0, 4, crc, payload);
    EXPECT_EQ(AstrOsBulkTransport::Decision::NAK, duplicate.decision);
    EXPECT_EQ(AstrOsBulkTransport::NakReason::OUT_OF_ORDER, duplicate.reason);
    // Reports we last committed seq=0, expect seq=1 next.
    EXPECT_EQ(0u, duplicate.highestContiguousSeq);
    EXPECT_EQ(1u, duplicate.nextExpectedSeq);
    EXPECT_EQ(16u, duplicate.windowRemaining); // active receiver -> full window
    EXPECT_EQ(nullptr, duplicate.payload);
    EXPECT_EQ(0u, duplicate.payloadLen);
}

TEST(BulkTransport, OnChunkSkipForwardNaksOutOfOrder)
{
    AstrOsBulkTransport::BulkReceiver r;
    ASSERT_TRUE(r.begin(/*xferId=*/7, /*totalSize=*/8, /*totalChunks=*/2, /*chunkSize=*/4, /*windowSize=*/16).valid);

    const uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};
    uint16_t crc = AstrOsBulkTransport::crc16_ccitt_false(payload, 4);

    // Sender jumps to seq=1 without sending seq=0.
    auto skipped = r.onChunk(7, 1, 4, crc, payload);
    EXPECT_EQ(AstrOsBulkTransport::Decision::NAK, skipped.decision);
    EXPECT_EQ(AstrOsBulkTransport::NakReason::OUT_OF_ORDER, skipped.reason);
    EXPECT_EQ(0u, skipped.nextExpectedSeq);
    EXPECT_EQ(16u, skipped.windowRemaining);
    EXPECT_EQ(nullptr, skipped.payload);
}

TEST(BulkTransport, OnChunkWrongXferIdNaksOutOfOrder)
{
    AstrOsBulkTransport::BulkReceiver r;
    ASSERT_TRUE(r.begin(/*xferId=*/7, /*totalSize=*/4, /*totalChunks=*/1, /*chunkSize=*/4, /*windowSize=*/16).valid);

    const uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};
    uint16_t crc = AstrOsBulkTransport::crc16_ccitt_false(payload, 4);

    // Send a chunk claiming xferId=9 on a receiver bound to xferId=7.
    auto result = r.onChunk(/*xferId=*/9, 0, 4, crc, payload);
    EXPECT_EQ(AstrOsBulkTransport::Decision::NAK, result.decision);
    EXPECT_EQ(AstrOsBulkTransport::NakReason::OUT_OF_ORDER, result.reason);
    EXPECT_EQ(0u, result.nextExpectedSeq);
    EXPECT_EQ(16u, result.windowRemaining);
    EXPECT_EQ(nullptr, result.payload);

    // The receiver state must not have been corrupted: a correct chunk for
    // xferId=7 still gets ACK'd cleanly afterward.
    auto recovery = r.onChunk(7, 0, 4, crc, payload);
    EXPECT_EQ(AstrOsBulkTransport::Decision::ACK, recovery.decision);
    EXPECT_EQ(1u, recovery.nextExpectedSeq);
}

//=================================================================================================
// BulkReceiver::onEnd — happy path
//=================================================================================================

TEST(BulkTransport, OnEndAfterAllChunksReturnsOk)
{
    AstrOsBulkTransport::BulkReceiver r;
    ASSERT_TRUE(r.begin(/*xferId=*/7, /*totalSize=*/8, /*totalChunks=*/2, /*chunkSize=*/4, /*windowSize=*/16).valid);

    const uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};
    uint16_t crc = AstrOsBulkTransport::crc16_ccitt_false(payload, 4);
    ASSERT_EQ(AstrOsBulkTransport::Decision::ACK, r.onChunk(7, 0, 4, crc, payload).decision);
    ASSERT_EQ(AstrOsBulkTransport::Decision::ACK, r.onChunk(7, 1, 4, crc, payload).decision);

    auto end = r.onEnd(/*xferId=*/7, /*totalChunksSent=*/2);
    EXPECT_EQ(AstrOsBulkTransport::EndResult::Status::OK, end.status);
}

//=================================================================================================
// BulkReceiver::onEnd — IO_ERROR reject paths
//=================================================================================================

TEST(BulkTransport, OnEndBeforeBeginReturnsIoError)
{
    AstrOsBulkTransport::BulkReceiver r;
    // No begin() called.
    auto end = r.onEnd(/*xferId=*/7, /*totalChunksSent=*/1);
    EXPECT_EQ(AstrOsBulkTransport::EndResult::Status::IO_ERROR, end.status);
}

TEST(BulkTransport, OnEndWrongXferIdReturnsIoError)
{
    AstrOsBulkTransport::BulkReceiver r;
    ASSERT_TRUE(r.begin(/*xferId=*/7, /*totalSize=*/4, /*totalChunks=*/1, /*chunkSize=*/4, /*windowSize=*/16).valid);

    const uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};
    uint16_t crc = AstrOsBulkTransport::crc16_ccitt_false(payload, 4);
    ASSERT_EQ(AstrOsBulkTransport::Decision::ACK, r.onChunk(7, 0, 4, crc, payload).decision);

    // Caller claims this END is for a different xferId.
    auto end = r.onEnd(/*xferId=*/9, /*totalChunksSent=*/1);
    EXPECT_EQ(AstrOsBulkTransport::EndResult::Status::IO_ERROR, end.status);
}

TEST(BulkTransport, OnEndSenderTotalMismatchReturnsIoError)
{
    AstrOsBulkTransport::BulkReceiver r;
    ASSERT_TRUE(r.begin(/*xferId=*/7, /*totalSize=*/8, /*totalChunks=*/2, /*chunkSize=*/4, /*windowSize=*/16).valid);

    const uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};
    uint16_t crc = AstrOsBulkTransport::crc16_ccitt_false(payload, 4);
    ASSERT_EQ(AstrOsBulkTransport::Decision::ACK, r.onChunk(7, 0, 4, crc, payload).decision);
    ASSERT_EQ(AstrOsBulkTransport::Decision::ACK, r.onChunk(7, 1, 4, crc, payload).decision);

    // Sender claims 3 chunks were sent, but begin() said 2.
    auto end = r.onEnd(/*xferId=*/7, /*totalChunksSent=*/3);
    EXPECT_EQ(AstrOsBulkTransport::EndResult::Status::IO_ERROR, end.status);
}

TEST(BulkTransport, OnEndReceiverShortChunkCountReturnsIoError)
{
    AstrOsBulkTransport::BulkReceiver r;
    ASSERT_TRUE(r.begin(/*xferId=*/7, /*totalSize=*/8, /*totalChunks=*/2, /*chunkSize=*/4, /*windowSize=*/16).valid);

    const uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};
    uint16_t crc = AstrOsBulkTransport::crc16_ccitt_false(payload, 4);
    ASSERT_EQ(AstrOsBulkTransport::Decision::ACK, r.onChunk(7, 0, 4, crc, payload).decision);
    // Only one chunk was sent; sender claims 2 to match the declared total.

    auto end = r.onEnd(/*xferId=*/7, /*totalChunksSent=*/2);
    EXPECT_EQ(AstrOsBulkTransport::EndResult::Status::IO_ERROR, end.status);
}

//=================================================================================================
// BulkReceiver — end-to-end happy path with one mid-stream CRC retransmit
//=================================================================================================

TEST(BulkTransport, EndToEndTenChunkTransferWithMidStreamRetransmit)
{
    constexpr uint32_t kTotalSize = 9500;
    constexpr uint16_t kChunkSize = 1024;
    constexpr uint32_t kTotalChunks = 10; // 9 full chunks + 1 short (9500 - 9216 = 284 bytes)
    constexpr uint8_t kXferId = 42;
    constexpr uint8_t kWindowSize = 16;

    AstrOsBulkTransport::BulkReceiver r;
    ASSERT_TRUE(r.begin(kXferId, kTotalSize, kTotalChunks, kChunkSize, kWindowSize).valid);

    // 9 full chunks + 1 short tail.
    std::vector<uint8_t> fullPayload(kChunkSize, 0xA5);
    uint16_t fullCrc = AstrOsBulkTransport::crc16_ccitt_false(fullPayload.data(), fullPayload.size());

    for (uint32_t seq = 0; seq < kTotalChunks - 1; seq++)
    {
        if (seq == 5)
        {
            // Mid-stream CRC error: sender sends correct payload with a corrupted CRC field.
            auto nakResult =
                r.onChunk(kXferId, seq, kChunkSize, static_cast<uint16_t>(fullCrc ^ 0x00FFu), fullPayload.data());
            ASSERT_EQ(AstrOsBulkTransport::Decision::NAK, nakResult.decision);
            ASSERT_EQ(AstrOsBulkTransport::NakReason::CRC, nakResult.reason);
            EXPECT_EQ(4u, nakResult.highestContiguousSeq);
            EXPECT_EQ(5u, nakResult.nextExpectedSeq);
        }
        // Falls through on every iteration: when seq == 5, this is the
        // retransmit with the correct CRC. Otherwise it is the normal send.
        auto okResult = r.onChunk(kXferId, seq, kChunkSize, fullCrc, fullPayload.data());
        ASSERT_EQ(AstrOsBulkTransport::Decision::ACK, okResult.decision) << "ACK failed at seq=" << seq;
        ASSERT_EQ(seq + 1, okResult.nextExpectedSeq);
    }

    // Final short chunk: 9500 - 9 * 1024 = 9500 - 9216 = 284 bytes.
    constexpr uint16_t kTailLen = 284;
    std::vector<uint8_t> tail(kTailLen, 0xC3);
    uint16_t tailCrc = AstrOsBulkTransport::crc16_ccitt_false(tail.data(), tail.size());
    auto tailResult = r.onChunk(kXferId, kTotalChunks - 1, kTailLen, tailCrc, tail.data());
    ASSERT_EQ(AstrOsBulkTransport::Decision::ACK, tailResult.decision);
    EXPECT_EQ(kTotalChunks - 1, tailResult.highestContiguousSeq);
    EXPECT_EQ(kTotalChunks, tailResult.nextExpectedSeq);

    // End of transfer.
    auto endResult = r.onEnd(kXferId, kTotalChunks);
    EXPECT_EQ(AstrOsBulkTransport::EndResult::Status::OK, endResult.status);

    r.reset();
}

//=================================================================================================
// BulkReceiver — end-to-end happy path with one mid-stream SIZE retransmit
//=================================================================================================

TEST(BulkTransport, EndToEndTransferWithMidStreamSizeRetransmit)
{
    constexpr uint32_t kTotalSize = 12;
    constexpr uint16_t kChunkSize = 4;
    constexpr uint32_t kTotalChunks = 3;
    constexpr uint8_t kXferId = 11;
    constexpr uint8_t kWindowSize = 16;

    AstrOsBulkTransport::BulkReceiver r;
    ASSERT_TRUE(r.begin(kXferId, kTotalSize, kTotalChunks, kChunkSize, kWindowSize).valid);

    const uint8_t fullPayload[] = {0xDE, 0xAD, 0xBE, 0xEF};
    uint16_t fullCrc = AstrOsBulkTransport::crc16_ccitt_false(fullPayload, 4);

    // seq=0 ACK clean.
    ASSERT_EQ(AstrOsBulkTransport::Decision::ACK, r.onChunk(kXferId, 0, 4, fullCrc, fullPayload).decision);

    // seq=1: sender erroneously claims 3 bytes (should be 4). NAK reason=SIZE.
    // CRC is computed over the (truncated) buffer the sender actually sends.
    const uint8_t shortPayload[] = {0xDE, 0xAD, 0xBE};
    uint16_t shortCrc = AstrOsBulkTransport::crc16_ccitt_false(shortPayload, 3);
    auto sizeNak = r.onChunk(kXferId, 1, 3, shortCrc, shortPayload);
    ASSERT_EQ(AstrOsBulkTransport::Decision::NAK, sizeNak.decision);
    ASSERT_EQ(AstrOsBulkTransport::NakReason::SIZE, sizeNak.reason);
    EXPECT_EQ(0u, sizeNak.highestContiguousSeq);
    EXPECT_EQ(1u, sizeNak.nextExpectedSeq);

    // Sender retransmits seq=1 with the correct 4-byte payload.
    ASSERT_EQ(AstrOsBulkTransport::Decision::ACK, r.onChunk(kXferId, 1, 4, fullCrc, fullPayload).decision);
    // seq=2 final.
    ASSERT_EQ(AstrOsBulkTransport::Decision::ACK, r.onChunk(kXferId, 2, 4, fullCrc, fullPayload).decision);

    auto end = r.onEnd(kXferId, kTotalChunks);
    EXPECT_EQ(AstrOsBulkTransport::EndResult::Status::OK, end.status);
}
