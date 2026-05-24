#include <AstrOsBulkTransport.hpp>
#include <gtest/gtest.h>

#include <algorithm>
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

TEST(BulkTransport, Crc16MultiByteNonStringVector)
{
    // Pin a multi-byte non-printable input to its known value. The canonical
    // "123456789" -> 0x29B1 catches polynomial swaps; this catches more subtle
    // bugs in the high-bit XOR feedback path where the polynomial is correct
    // but, say, the shift direction or XOR mask gets flipped — those would
    // still produce a deterministic-but-wrong value, which the determinism
    // test alone would not detect.
    const uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF};
    EXPECT_EQ(0x4097u, AstrOsBulkTransport::crc16_ccitt_false(data, 4));
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

TEST(BulkTransport, ResetThenBeginAcceptsNewTransfer)
{
    // Pins that reset() returns the receiver to pre-begin state cleanly:
    // a follow-up begin() with a different xferId activates a fresh
    // transfer, and the first onChunk on that new transfer ACKs with
    // the resume cursor reinitialized to (highestContiguousSeq=0,
    // nextExpectedSeq=1).
    //
    // NOTE: this test does NOT cover the begin-after-end case (commit
    // a successful transfer end-to-end, then begin a new one). That's
    // covered by OnEndOkLeavesReceiverInPostOkState in combination with
    // BeginOnActiveReceiverReinitializes (which pins that begin() on
    // an already-active receiver reinit's correctly).
    AstrOsBulkTransport::BulkReceiver r;
    ASSERT_TRUE(r.begin(/*xferId=*/7, /*totalSize=*/4, /*totalChunks=*/1, /*chunkSize=*/4, /*windowSize=*/16).valid);

    const uint8_t firstPayload[] = {'a', 'b', 'c', 'd'};
    uint16_t firstCrc = AstrOsBulkTransport::crc16_ccitt_false(firstPayload, 4);
    auto first = r.onChunk(7, 0, 4, firstCrc, firstPayload);
    ASSERT_EQ(AstrOsBulkTransport::Decision::ACK, first.decision);

    // Reset (without ending the transfer first), begin a new transfer
    // with a different xferId, and accept its first chunk.
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
    // Pin payload pass-through on the short-tail chunk specifically: this is
    // the only seq where the SIZE math computes a non-full expectedLen, and a
    // hypothetical bug that assigned chunkSize_ to payloadLen instead of the
    // caller's payloadLen would silently corrupt the final flash write.
    EXPECT_EQ(tail.data(), r2.payload);
    EXPECT_EQ(452u, r2.payloadLen);
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

TEST(BulkTransport, OnChunkNullPayloadWithPositiveLenNaksOutOfOrder)
{
    // Regression guard for a real bug: crc16_ccitt_false's empty-input case
    // returns 0xFFFFu. The naive `if (data == nullptr || len == 0)` form of
    // that guard made (nullptr, K, 0xFFFFu) look like a valid ACK candidate
    // because:
    //   - payloadLen=K matches expectedLen (SIZE check passes),
    //   - crc16_ccitt_false(nullptr, K) returns 0xFFFFu (CRC check passes),
    //   - onChunk returns ACK with payload=nullptr → Phase 3 derefs null.
    // Closed by the upstream structural guard in onChunk and by tightening
    // crc16_ccitt_false to only short-circuit on len==0. This test pins
    // both: a nullptr payload with positive payloadLen MUST NAK, and the
    // ChunkResult MUST NOT carry a non-null payload pointer.
    AstrOsBulkTransport::BulkReceiver r;
    ASSERT_TRUE(r.begin(/*xferId=*/7, /*totalSize=*/4, /*totalChunks=*/1, /*chunkSize=*/4, /*windowSize=*/16).valid);

    auto result = r.onChunk(/*xferId=*/7, /*seq=*/0, /*payloadLen=*/4, /*crc16=*/0xFFFFu, /*payload=*/nullptr);
    EXPECT_EQ(AstrOsBulkTransport::Decision::NAK, result.decision);
    EXPECT_EQ(AstrOsBulkTransport::NakReason::OUT_OF_ORDER, result.reason);
    EXPECT_EQ(nullptr, result.payload);
    EXPECT_EQ(0u, result.payloadLen);
    EXPECT_EQ(0u, result.nextExpectedSeq); // first-chunk sentinel
    EXPECT_EQ(16u, result.windowRemaining);
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
    EXPECT_EQ(AstrOsBulkTransport::EndResult::Reason::NOT_ACTIVE, end.reason);
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
    EXPECT_EQ(AstrOsBulkTransport::EndResult::Reason::WRONG_XFER_ID, end.reason);
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
    EXPECT_EQ(AstrOsBulkTransport::EndResult::Reason::SENDER_TOTAL_MISMATCH, end.reason);
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
    // RECEIVER_SHORT_COUNT is distinct from SENDER_TOTAL_MISMATCH: here the
    // sender's totalChunksSent agrees with the receiver's declared total,
    // but the receiver actually committed fewer chunks (probably lost some
    // to NAKs the sender didn't resend). Phase 3 needs the distinction so
    // the operator log says "lost some chunks" vs "sender lied about total."
    EXPECT_EQ(AstrOsBulkTransport::EndResult::Reason::RECEIVER_SHORT_COUNT, end.reason);
}

TEST(BulkTransport, OnEndOkLeavesReceiverInPostOkState)
{
    // Contract pin: onEnd(OK) does NOT auto-reset the receiver. nextSeq_
    // is left at totalChunks_, active_ stays true. The state machine remains
    // queryable via onChunk — a stray retransmit chunk arriving after a
    // successful END must produce a deterministic, well-defined NAK rather
    // than corrupt internal state. Phase 3 is responsible for calling
    // reset() before begin()-ing the next transfer (which begin() itself
    // does internally on reinit, per the BeginOnActiveReceiverReinitializes
    // contract).
    AstrOsBulkTransport::BulkReceiver r;
    ASSERT_TRUE(r.begin(/*xferId=*/7, /*totalSize=*/8, /*totalChunks=*/2, /*chunkSize=*/4, /*windowSize=*/16).valid);
    const uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};
    uint16_t crc = AstrOsBulkTransport::crc16_ccitt_false(payload, 4);
    ASSERT_EQ(AstrOsBulkTransport::Decision::ACK, r.onChunk(7, 0, 4, crc, payload).decision);
    ASSERT_EQ(AstrOsBulkTransport::Decision::ACK, r.onChunk(7, 1, 4, crc, payload).decision);
    ASSERT_EQ(AstrOsBulkTransport::EndResult::Status::OK, r.onEnd(7, 2).status);

    // A stray retransmit (seq=2 = totalChunks_) arrives after the OK END.
    // The seq >= totalChunks_ structural guard from the hardening commit
    // catches it as OUT_OF_ORDER, with the active-state sync fields
    // (windowRemaining = configured windowSize, nextExpectedSeq = nextSeq_).
    auto stray = r.onChunk(7, 2, 4, crc, payload);
    EXPECT_EQ(AstrOsBulkTransport::Decision::NAK, stray.decision);
    EXPECT_EQ(AstrOsBulkTransport::NakReason::OUT_OF_ORDER, stray.reason);
    EXPECT_EQ(16u, stray.windowRemaining); // still active — NOT the inactive sentinel
    EXPECT_EQ(2u, stray.nextExpectedSeq);  // == totalChunks_; tells sender "no more chunks"

    // A second onEnd is idempotent: returns OK again because all totals
    // still match. This isn't a "transfer finished twice" — it just means
    // the contract is "OK is the steady state after success."
    auto secondEnd = r.onEnd(7, 2);
    EXPECT_EQ(AstrOsBulkTransport::EndResult::Status::OK, secondEnd.status);
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

// shouldTeardownOnEndResult pins the gate that OtaReceiver::handleEnd uses to
// decide whether to clear in-flight state. A regression that flipped a branch
// here would silently re-introduce the round-1 C1 bug (stray END clobbering
// a healthy transfer). One test per Reason value, plus the OK happy path.

TEST(BulkTransport, ShouldTeardownOnEndResultOkTearsDown)
{
    EXPECT_TRUE(AstrOsBulkTransport::shouldTeardownOnEndResult(AstrOsBulkTransport::EndResult::ok()));
}

TEST(BulkTransport, ShouldTeardownOnEndResultSenderTotalMismatchTearsDown)
{
    EXPECT_TRUE(AstrOsBulkTransport::shouldTeardownOnEndResult(
        AstrOsBulkTransport::EndResult::ioError(AstrOsBulkTransport::EndResult::Reason::SENDER_TOTAL_MISMATCH)));
}

TEST(BulkTransport, ShouldTeardownOnEndResultReceiverShortCountTearsDown)
{
    EXPECT_TRUE(AstrOsBulkTransport::shouldTeardownOnEndResult(
        AstrOsBulkTransport::EndResult::ioError(AstrOsBulkTransport::EndResult::Reason::RECEIVER_SHORT_COUNT)));
}

TEST(BulkTransport, ShouldTeardownOnEndResultWrongXferIdPreservesState)
{
    EXPECT_FALSE(AstrOsBulkTransport::shouldTeardownOnEndResult(
        AstrOsBulkTransport::EndResult::ioError(AstrOsBulkTransport::EndResult::Reason::WRONG_XFER_ID)));
}

TEST(BulkTransport, ShouldTeardownOnEndResultNotActivePreservesState)
{
    EXPECT_FALSE(AstrOsBulkTransport::shouldTeardownOnEndResult(
        AstrOsBulkTransport::EndResult::ioError(AstrOsBulkTransport::EndResult::Reason::NOT_ACTIVE)));
}

//=================================================================================================
// BulkSender — M2 wire-format-compatible sender state machine.
//
// Mirrors the BulkReceiver result-type family: each result struct carries
// [[nodiscard]] at the type level + file-scope static_assert pinning of
// integer enum values. BeginSenderResult / BeginAckResult use the simpler
// BeginResult / EndResult pattern (mutable bool valid + Reason fields,
// static factories). Later M2 tasks introduce ChunkResult-style
// const-fields + private-ctor result types (SendResult, AckResult, NakResult,
// EndAckResult) where the additional invariant enforcement earns its keep.
//=================================================================================================

TEST(BulkTransport, BulkSenderStartsInIdle)
{
    AstrOsBulkTransport::BulkSender s;
    EXPECT_EQ(AstrOsBulkTransport::BulkSender::Status::IDLE, s.status());
}

TEST(BulkTransport, BulkSenderBeginAcceptsValidParams)
{
    AstrOsBulkTransport::BulkSender s;
    auto r = s.begin(/*xferId=*/7, /*totalChunks=*/100, /*chunkSize=*/128,
                     /*windowSize=*/8, /*ackTimeoutMs=*/400, /*maxRetries=*/3);
    EXPECT_TRUE(r.valid);
    EXPECT_EQ(AstrOsBulkTransport::BeginSenderResult::Reason::OK, r.reason);
    EXPECT_EQ(AstrOsBulkTransport::BulkSender::Status::AWAITING_BEGIN_ACK, s.status());
}

TEST(BulkTransport, BulkSenderBeginRejectsZeroTotalChunks)
{
    AstrOsBulkTransport::BulkSender s;
    auto r = s.begin(/*xferId=*/7, /*totalChunks=*/0, /*chunkSize=*/128,
                     /*windowSize=*/8, /*ackTimeoutMs=*/400, /*maxRetries=*/3);
    EXPECT_FALSE(r.valid);
    EXPECT_EQ(AstrOsBulkTransport::BeginSenderResult::Reason::ZERO_TOTAL_CHUNKS, r.reason);
    EXPECT_EQ(AstrOsBulkTransport::BulkSender::Status::IDLE, s.status());
}

TEST(BulkTransport, BulkSenderBeginRejectsZeroChunkSize)
{
    AstrOsBulkTransport::BulkSender s;
    auto r = s.begin(7, 100, 0, 8, 400, 3);
    EXPECT_FALSE(r.valid);
    EXPECT_EQ(AstrOsBulkTransport::BeginSenderResult::Reason::ZERO_CHUNK_SIZE, r.reason);
}

TEST(BulkTransport, BulkSenderBeginRejectsZeroWindowSize)
{
    AstrOsBulkTransport::BulkSender s;
    auto r = s.begin(7, 100, 128, 0, 400, 3);
    EXPECT_FALSE(r.valid);
    EXPECT_EQ(AstrOsBulkTransport::BeginSenderResult::Reason::ZERO_WINDOW_SIZE, r.reason);
}

TEST(BulkTransport, BulkSenderBeginRejectsZeroAckTimeout)
{
    AstrOsBulkTransport::BulkSender s;
    auto r = s.begin(7, 100, 128, 8, 0, 3);
    EXPECT_FALSE(r.valid);
    EXPECT_EQ(AstrOsBulkTransport::BeginSenderResult::Reason::ZERO_ACK_TIMEOUT, r.reason);
}

TEST(BulkTransport, BulkSenderBeginRejectsZeroMaxRetries)
{
    AstrOsBulkTransport::BulkSender s;
    auto r = s.begin(7, 100, 128, 8, 400, 0);
    EXPECT_FALSE(r.valid);
    EXPECT_EQ(AstrOsBulkTransport::BeginSenderResult::Reason::ZERO_MAX_RETRIES, r.reason);
}

TEST(BulkTransport, BulkSenderBeginRejectsWindowTooLarge)
{
    AstrOsBulkTransport::BulkSender s;
    // MAX_WINDOW_SIZE = 16; windowSize = 17 must reject.
    auto r = s.begin(7, 100, 128, 17, 400, 3);
    EXPECT_FALSE(r.valid);
    EXPECT_EQ(AstrOsBulkTransport::BeginSenderResult::Reason::WINDOW_TOO_LARGE, r.reason);
}

TEST(BulkTransport, BulkSenderOnBeginAckAdvancesToStreaming)
{
    AstrOsBulkTransport::BulkSender s;
    ASSERT_TRUE(s.begin(7, 100, 128, 8, 400, 3).valid);
    auto r = s.onBeginAck(/*xferId=*/7);
    EXPECT_EQ(AstrOsBulkTransport::BeginAckResult::Decision::OK, r.decision);
    EXPECT_EQ(AstrOsBulkTransport::BulkSender::Status::STREAMING, s.status());
}

TEST(BulkTransport, BulkSenderOnBeginAckRejectsWrongXferId)
{
    AstrOsBulkTransport::BulkSender s;
    ASSERT_TRUE(s.begin(7, 100, 128, 8, 400, 3).valid);
    auto r = s.onBeginAck(/*xferId=*/99);
    EXPECT_EQ(AstrOsBulkTransport::BeginAckResult::Decision::WRONG_XFER_ID, r.decision);
    EXPECT_EQ(AstrOsBulkTransport::BulkSender::Status::AWAITING_BEGIN_ACK, s.status());
}

TEST(BulkTransport, BulkSenderOnBeginAckRejectsBeforeBegin)
{
    AstrOsBulkTransport::BulkSender s;
    // No begin() called — status is IDLE.
    auto r = s.onBeginAck(7);
    EXPECT_EQ(AstrOsBulkTransport::BeginAckResult::Decision::NOT_AWAITING_BEGIN_ACK, r.decision);
}

TEST(BulkTransport, BulkSenderResetReturnsToIdle)
{
    AstrOsBulkTransport::BulkSender s;
    ASSERT_TRUE(s.begin(7, 100, 128, 8, 400, 3).valid);
    ASSERT_EQ(AstrOsBulkTransport::BulkSender::Status::AWAITING_BEGIN_ACK, s.status());
    s.reset();
    EXPECT_EQ(AstrOsBulkTransport::BulkSender::Status::IDLE, s.status());
}

namespace
{
    // Helper: drive `nextChunkToSend` in a loop until WINDOW_FULL or ALL_SENT.
    // Records each emitted seq in `outSeqs`. Returns the final Decision.
    AstrOsBulkTransport::SendResult::Decision drainSends(AstrOsBulkTransport::BulkSender &s, uint64_t nowMs,
                                                         std::vector<uint32_t> &outSeqs)
    {
        for (;;)
        {
            auto r = s.nextChunkToSend(nowMs);
            if (r.decision == AstrOsBulkTransport::SendResult::Decision::SEND)
            {
                outSeqs.push_back(r.seq);
                continue;
            }
            return r.decision;
        }
    }
} // namespace

TEST(BulkTransport, BulkSenderNextChunkRejectedBeforeStreaming)
{
    AstrOsBulkTransport::BulkSender s;
    auto r = s.nextChunkToSend(/*nowMs=*/0);
    EXPECT_EQ(AstrOsBulkTransport::SendResult::Decision::NOT_STREAMING, r.decision);
}

TEST(BulkTransport, BulkSenderNextChunkAfterBeginAckEmitsSeqZero)
{
    AstrOsBulkTransport::BulkSender s;
    ASSERT_TRUE(s.begin(7, 100, 128, 8, 400, 3).valid);
    ASSERT_EQ(AstrOsBulkTransport::BeginAckResult::Decision::OK, s.onBeginAck(7).decision);

    auto r = s.nextChunkToSend(/*nowMs=*/1000);
    EXPECT_EQ(AstrOsBulkTransport::SendResult::Decision::SEND, r.decision);
    EXPECT_EQ(0u, r.seq);
}

TEST(BulkTransport, BulkSenderFillsWindowThenStopsWithWindowFull)
{
    AstrOsBulkTransport::BulkSender s;
    ASSERT_TRUE(s.begin(7, /*totalChunks=*/100, 128, /*windowSize=*/8, 400, 3).valid);
    ASSERT_EQ(AstrOsBulkTransport::BeginAckResult::Decision::OK, s.onBeginAck(7).decision);

    std::vector<uint32_t> sent;
    auto terminal = drainSends(s, 1000, sent);

    EXPECT_EQ(AstrOsBulkTransport::SendResult::Decision::WINDOW_FULL, terminal);
    ASSERT_EQ(8u, sent.size());
    for (uint32_t i = 0; i < 8; i++)
    {
        EXPECT_EQ(i, sent[i]);
    }
}

TEST(BulkTransport, BulkSenderEmitsAllChunksWhenTotalIsBelowWindow)
{
    AstrOsBulkTransport::BulkSender s;
    ASSERT_TRUE(s.begin(7, /*totalChunks=*/3, 128, /*windowSize=*/8, 400, 3).valid);
    ASSERT_EQ(AstrOsBulkTransport::BeginAckResult::Decision::OK, s.onBeginAck(7).decision);

    std::vector<uint32_t> sent;
    auto terminal = drainSends(s, 1000, sent);

    EXPECT_EQ(AstrOsBulkTransport::SendResult::Decision::ALL_SENT, terminal);
    ASSERT_EQ(3u, sent.size());
    EXPECT_EQ(0u, sent[0]);
    EXPECT_EQ(1u, sent[1]);
    EXPECT_EQ(2u, sent[2]);
}

TEST(BulkTransport, BulkSenderOnDataAckAdvancesHighestConfirmed)
{
    AstrOsBulkTransport::BulkSender s;
    ASSERT_TRUE(s.begin(7, 100, 128, 8, 400, 3).valid);
    ASSERT_EQ(AstrOsBulkTransport::BeginAckResult::Decision::OK, s.onBeginAck(7).decision);

    std::vector<uint32_t> sent;
    drainSends(s, 1000, sent);
    ASSERT_EQ(8u, sent.size()); // window full at 0..7

    // Cumulative ACK up through seq 3: frees slots for 0..3.
    auto r = s.onDataAck(7, /*cumulativeSeq=*/3);
    EXPECT_EQ(AstrOsBulkTransport::AckResult::Decision::OK, r.decision);
    EXPECT_EQ(4u, r.newlyConfirmedCount); // seqs 0, 1, 2, 3 newly confirmed

    // Now nextChunkToSend should emit seq 8 (window frees 4 slots).
    auto sr = s.nextChunkToSend(1100);
    EXPECT_EQ(AstrOsBulkTransport::SendResult::Decision::SEND, sr.decision);
    EXPECT_EQ(8u, sr.seq);
}

TEST(BulkTransport, BulkSenderRejectsStaleAck)
{
    AstrOsBulkTransport::BulkSender s;
    ASSERT_TRUE(s.begin(7, 100, 128, 8, 400, 3).valid);
    ASSERT_EQ(AstrOsBulkTransport::BeginAckResult::Decision::OK, s.onBeginAck(7).decision);

    std::vector<uint32_t> sent;
    drainSends(s, 1000, sent);

    ASSERT_EQ(AstrOsBulkTransport::AckResult::Decision::OK, s.onDataAck(7, 5).decision);
    // Stale: cumulativeSeq=3 already covered by the previous ACK at 5.
    auto r = s.onDataAck(7, 3);
    EXPECT_EQ(AstrOsBulkTransport::AckResult::Decision::STALE, r.decision);
}

TEST(BulkTransport, BulkSenderOnDataAckRejectsWrongXferId)
{
    AstrOsBulkTransport::BulkSender s;
    ASSERT_TRUE(s.begin(7, 100, 128, 8, 400, 3).valid);
    ASSERT_EQ(AstrOsBulkTransport::BeginAckResult::Decision::OK, s.onBeginAck(7).decision);
    auto r = s.onDataAck(/*xferId=*/99, 3);
    EXPECT_EQ(AstrOsBulkTransport::AckResult::Decision::WRONG_XFER_ID, r.decision);
}

TEST(BulkTransport, BulkSenderOnDataAckRejectsBeforeStreaming)
{
    AstrOsBulkTransport::BulkSender s;
    // No begin / onBeginAck — status is IDLE.
    auto r = s.onDataAck(7, 0);
    EXPECT_EQ(AstrOsBulkTransport::AckResult::Decision::NOT_STREAMING, r.decision);
}

TEST(BulkTransport, BulkSenderOnDataNakRewindsToNextExpectedSeq)
{
    AstrOsBulkTransport::BulkSender s;
    ASSERT_TRUE(s.begin(7, 100, 128, 8, 400, 3).valid);
    ASSERT_EQ(AstrOsBulkTransport::BeginAckResult::Decision::OK, s.onBeginAck(7).decision);

    std::vector<uint32_t> sent;
    drainSends(s, 1000, sent);
    ASSERT_EQ(8u, sent.size()); // sent 0..7

    // Receiver NAKs with nextExpectedSeq=3 — meaning seqs 0..2 are OK,
    // but seq 3 needs retransmission (and 4..7 will be discarded by
    // the receiver since they're out of order).
    auto r = s.onDataNak(7, /*nextExpectedSeq=*/3, AstrOsBulkTransport::NakReason::CRC);
    EXPECT_EQ(AstrOsBulkTransport::NakResult::Decision::OK, r.decision);
    EXPECT_EQ(3u, r.nextSeqToResend);

    // The next send should re-emit seq 3 (not 8).
    auto sr = s.nextChunkToSend(1100);
    EXPECT_EQ(AstrOsBulkTransport::SendResult::Decision::SEND, sr.decision);
    EXPECT_EQ(3u, sr.seq);
}

TEST(BulkTransport, BulkSenderOnDataNakRejectsWrongXferId)
{
    AstrOsBulkTransport::BulkSender s;
    ASSERT_TRUE(s.begin(7, 100, 128, 8, 400, 3).valid);
    ASSERT_EQ(AstrOsBulkTransport::BeginAckResult::Decision::OK, s.onBeginAck(7).decision);
    auto r = s.onDataNak(/*xferId=*/99, 3, AstrOsBulkTransport::NakReason::CRC);
    EXPECT_EQ(AstrOsBulkTransport::NakResult::Decision::WRONG_XFER_ID, r.decision);
}

TEST(BulkTransport, BulkSenderOnDataNakRejectsBeforeStreaming)
{
    // Symmetry with the OnDataAck variant: NAK with status==IDLE returns
    // NOT_STREAMING. Closes the test-matrix gap (ACK has this test; NAK
    // didn't until now).
    AstrOsBulkTransport::BulkSender s;
    auto r = s.onDataNak(7, 0, AstrOsBulkTransport::NakReason::CRC);
    EXPECT_EQ(AstrOsBulkTransport::NakResult::Decision::NOT_STREAMING, r.decision);
}

TEST(BulkTransport, BulkSenderOnDataNakImplicitlyAdvancesConfirmedWatermark)
{
    // NAK with nextExpectedSeq=3 implies seqs 0..2 are confirmed. A
    // subsequent ACK at cumulativeSeq=2 must be rejected as stale — the
    // implicit confirmation already covered that range. This pins the
    // cumulative-ACK semantics of NAK so a future regression that drops
    // the highestConfirmedSeq_ advance gets caught.
    AstrOsBulkTransport::BulkSender s;
    ASSERT_TRUE(s.begin(7, 100, 128, 8, 400, 3).valid);
    ASSERT_EQ(AstrOsBulkTransport::BeginAckResult::Decision::OK, s.onBeginAck(7).decision);

    std::vector<uint32_t> sent;
    drainSends(s, 1000, sent);

    ASSERT_EQ(AstrOsBulkTransport::NakResult::Decision::OK,
              s.onDataNak(7, /*nextExpectedSeq=*/3, AstrOsBulkTransport::NakReason::CRC).decision);

    auto staleR = s.onDataAck(7, 2);
    EXPECT_EQ(AstrOsBulkTransport::AckResult::Decision::STALE, staleR.decision);
}

TEST(BulkTransport, BulkSenderOnDataNakWithZeroNextExpectedDoesNotAdvanceConfirmed)
{
    // NAK with nextExpectedSeq=0 means "receiver got nothing" — no implicit
    // confirmation. Subsequent ACK at cumulativeSeq=0 must NOT be stale;
    // the NAK didn't confirm anything.
    AstrOsBulkTransport::BulkSender s;
    ASSERT_TRUE(s.begin(7, 100, 128, 8, 400, 3).valid);
    ASSERT_EQ(AstrOsBulkTransport::BeginAckResult::Decision::OK, s.onBeginAck(7).decision);

    std::vector<uint32_t> sent;
    drainSends(s, 1000, sent);

    ASSERT_EQ(AstrOsBulkTransport::NakResult::Decision::OK,
              s.onDataNak(7, /*nextExpectedSeq=*/0, AstrOsBulkTransport::NakReason::CRC).decision);

    auto ackR = s.onDataAck(7, 0);
    EXPECT_EQ(AstrOsBulkTransport::AckResult::Decision::OK, ackR.decision);
}

TEST(BulkTransport, BulkSenderTickReturnsNothingWhenNoTimeoutsFired)
{
    AstrOsBulkTransport::BulkSender s;
    ASSERT_TRUE(s.begin(7, 100, 128, 8, /*ackTimeoutMs=*/400, /*maxRetries=*/3).valid);
    ASSERT_EQ(AstrOsBulkTransport::BeginAckResult::Decision::OK, s.onBeginAck(7).decision);

    std::vector<uint32_t> sent;
    drainSends(s, /*nowMs=*/1000, sent);
    ASSERT_EQ(8u, sent.size());

    auto t = s.tick(/*nowMs=*/1100); // only 100 ms passed — under the 400 ms timeout
    EXPECT_EQ(0, t.count);
    EXPECT_FALSE(t.abandon);
}

TEST(BulkTransport, BulkSenderTickRetransmitsTimedOutSeqs)
{
    AstrOsBulkTransport::BulkSender s;
    ASSERT_TRUE(s.begin(7, 100, 128, 8, 400, 3).valid);
    ASSERT_EQ(AstrOsBulkTransport::BeginAckResult::Decision::OK, s.onBeginAck(7).decision);

    std::vector<uint32_t> sent;
    drainSends(s, 1000, sent);

    auto t = s.tick(/*nowMs=*/1500); // 500 ms passed — past the 400 ms timeout
    EXPECT_EQ(8, t.count);
    EXPECT_FALSE(t.abandon);
    // All 8 in-flight seqs should be flagged for retransmission.
    for (uint8_t i = 0; i < 8; i++)
    {
        EXPECT_EQ(i, t.retransmitSeqs[i]);
    }
}

TEST(BulkTransport, BulkSenderTickRetransmitsOnlyPerEntryTimedOutSeqs)
{
    // Per-entry timestamp granularity: send 4 chunks at t=1000, ACK one of them,
    // send 3 more at t=1200 (so in-flight has mixed timestamps from t=1000 and
    // t=1200). Tick at t=1450 with ackTimeout=400. Only the t=1000 seqs are
    // past timeout (1450-1000=450 >= 400); t=1200 seqs aren't (1450-1200=250 < 400).
    //
    // Catches a regression where tick uses a single global timer instead of
    // per-entry sendTimestampMs.
    AstrOsBulkTransport::BulkSender s;
    ASSERT_TRUE(s.begin(7, 100, 128, 8, /*ackTimeoutMs=*/400, 3).valid);
    ASSERT_EQ(AstrOsBulkTransport::BeginAckResult::Decision::OK, s.onBeginAck(7).decision);

    // Send seqs 0..3 at t=1000.
    std::vector<uint32_t> firstBatch;
    for (uint32_t i = 0; i < 4; i++)
    {
        auto r = s.nextChunkToSend(1000);
        ASSERT_EQ(AstrOsBulkTransport::SendResult::Decision::SEND, r.decision);
        firstBatch.push_back(r.seq);
    }
    ASSERT_EQ(4u, firstBatch.size());

    // ACK seq 0 — frees one slot. In-flight now {1, 2, 3} at t=1000.
    ASSERT_EQ(AstrOsBulkTransport::AckResult::Decision::OK, s.onDataAck(7, 0).decision);

    // Send seqs 4..6 at t=1200. In-flight now {1, 2, 3} at t=1000 + {4, 5, 6} at t=1200.
    for (uint32_t i = 0; i < 3; i++)
    {
        auto r = s.nextChunkToSend(1200);
        ASSERT_EQ(AstrOsBulkTransport::SendResult::Decision::SEND, r.decision);
    }

    // Tick at t=1450. Slots from t=1000: 450 ms elapsed (>= 400, timed out).
    //                Slots from t=1200: 250 ms elapsed (< 400, not timed out).
    auto t = s.tick(1450);
    EXPECT_EQ(3, t.count); // only seqs 1, 2, 3 — NOT seqs 4, 5, 6
    EXPECT_FALSE(t.abandon);
    // Per-seq timestamp drives the decision, not a global timer.
    // Iteration order is slot-array order; collect into a sorted vector for stable comparison.
    std::vector<uint32_t> retransmitted(t.retransmitSeqs.begin(), t.retransmitSeqs.begin() + t.count);
    std::sort(retransmitted.begin(), retransmitted.end());
    EXPECT_EQ(1u, retransmitted[0]);
    EXPECT_EQ(2u, retransmitted[1]);
    EXPECT_EQ(3u, retransmitted[2]);
}

TEST(BulkTransport, BulkSenderTickAbandonsAfterMaxRetries)
{
    AstrOsBulkTransport::BulkSender s;
    ASSERT_TRUE(s.begin(7, 100, 128, 8, /*ackTimeoutMs=*/400, /*maxRetries=*/2).valid);
    ASSERT_EQ(AstrOsBulkTransport::BeginAckResult::Decision::OK, s.onBeginAck(7).decision);

    std::vector<uint32_t> sent;
    drainSends(s, 0, sent);

    // First tick at 500 ms: retryCount 0 → 1 for every seq.
    auto t1 = s.tick(500);
    EXPECT_EQ(8, t1.count);
    EXPECT_FALSE(t1.abandon);

    // Second tick at 1000 ms: retryCount 1 → 2 for every seq.
    auto t2 = s.tick(1000);
    EXPECT_EQ(8, t2.count);
    EXPECT_FALSE(t2.abandon);

    // Third tick at 1500 ms: retryCount would go 2 → 3 (> maxRetries=2).
    // Abandon, no further retransmissions.
    auto t3 = s.tick(1500);
    EXPECT_TRUE(t3.abandon);
    EXPECT_EQ(0, t3.count); // abandon path resets count, not a partial retransmit list
    EXPECT_EQ(AstrOsBulkTransport::BulkSender::Status::ABANDONED, s.status());
}

TEST(BulkTransport, BulkSenderTickReturnsZeroOnInactive)
{
    AstrOsBulkTransport::BulkSender s;
    // Never called begin — IDLE.
    auto t = s.tick(10000);
    EXPECT_EQ(0, t.count);
    EXPECT_FALSE(t.abandon);
}

TEST(BulkTransport, BulkSenderOnEndAckOkAfterFullDrainTransitionsToDoneOk)
{
    // Use a small totalChunks so we can drain + ACK all of them before onEndAck.
    // The PREMATURE check now requires the implicit AWAITING_END_ACK predicate
    // (all chunks sent + all confirmed) to be satisfied first.
    AstrOsBulkTransport::BulkSender s;
    ASSERT_TRUE(s.begin(7, /*totalChunks=*/3, 128, /*windowSize=*/8, 400, 3).valid);
    ASSERT_EQ(AstrOsBulkTransport::BeginAckResult::Decision::OK, s.onBeginAck(7).decision);

    std::vector<uint32_t> sent;
    auto term = drainSends(s, 1000, sent);
    ASSERT_EQ(AstrOsBulkTransport::SendResult::Decision::ALL_SENT, term);
    ASSERT_EQ(3u, sent.size());

    ASSERT_EQ(AstrOsBulkTransport::AckResult::Decision::OK, s.onDataAck(7, /*cumulativeSeq=*/2).decision);

    auto r = s.onEndAck(7, OtaEndStatus::OK);
    EXPECT_EQ(AstrOsBulkTransport::EndAckResult::Decision::DONE_OK, r.decision);
    EXPECT_EQ(AstrOsBulkTransport::BulkSender::Status::DONE_OK, s.status());
}

TEST(BulkTransport, BulkSenderOnEndAckHashMismatchAfterFullDrainAbandons)
{
    AstrOsBulkTransport::BulkSender s;
    ASSERT_TRUE(s.begin(7, /*totalChunks=*/3, 128, /*windowSize=*/8, 400, 3).valid);
    ASSERT_EQ(AstrOsBulkTransport::BeginAckResult::Decision::OK, s.onBeginAck(7).decision);

    std::vector<uint32_t> sent;
    drainSends(s, 1000, sent);
    ASSERT_EQ(AstrOsBulkTransport::AckResult::Decision::OK, s.onDataAck(7, 2).decision);

    auto r = s.onEndAck(7, OtaEndStatus::HASH_MISMATCH);
    EXPECT_EQ(AstrOsBulkTransport::EndAckResult::Decision::ABANDONED, r.decision);
    EXPECT_EQ(AstrOsBulkTransport::BulkSender::Status::ABANDONED, s.status());
}

TEST(BulkTransport, BulkSenderOnEndAckWriteErrorAfterFullDrainAbandons)
{
    AstrOsBulkTransport::BulkSender s;
    ASSERT_TRUE(s.begin(7, /*totalChunks=*/3, 128, /*windowSize=*/8, 400, 3).valid);
    ASSERT_EQ(AstrOsBulkTransport::BeginAckResult::Decision::OK, s.onBeginAck(7).decision);

    std::vector<uint32_t> sent;
    drainSends(s, 1000, sent);
    ASSERT_EQ(AstrOsBulkTransport::AckResult::Decision::OK, s.onDataAck(7, 2).decision);

    auto r = s.onEndAck(7, OtaEndStatus::WRITE_ERROR);
    EXPECT_EQ(AstrOsBulkTransport::EndAckResult::Decision::ABANDONED, r.decision);
    EXPECT_EQ(AstrOsBulkTransport::BulkSender::Status::ABANDONED, s.status());
}

TEST(BulkTransport, BulkSenderOnEndAckRejectsWrongXferId)
{
    AstrOsBulkTransport::BulkSender s;
    ASSERT_TRUE(s.begin(7, 100, 128, 8, 400, 3).valid);
    ASSERT_EQ(AstrOsBulkTransport::BeginAckResult::Decision::OK, s.onBeginAck(7).decision);
    auto r = s.onEndAck(/*xferId=*/99, OtaEndStatus::OK);
    EXPECT_EQ(AstrOsBulkTransport::EndAckResult::Decision::WRONG_XFER_ID, r.decision);
    EXPECT_EQ(AstrOsBulkTransport::BulkSender::Status::STREAMING, s.status());
}

TEST(BulkTransport, BulkSenderOnEndAckRejectsBeforeStreaming)
{
    // Symmetry with the NOT_STREAMING tests for the other BulkSender
    // methods. Closes the test-matrix gap so a future refactor that
    // accidentally drops the status guard from onEndAck is caught.
    AstrOsBulkTransport::BulkSender s;
    // Never called begin — status is IDLE.
    auto r = s.onEndAck(7, OtaEndStatus::OK);
    EXPECT_EQ(AstrOsBulkTransport::EndAckResult::Decision::NOT_STREAMING, r.decision);
    EXPECT_EQ(AstrOsBulkTransport::BulkSender::Status::IDLE, s.status());
}

TEST(BulkTransport, BulkSenderOnDataAckRejectsOutOfRangeCumulativeSeq)
{
    // Defensive: cumulativeSeq >= totalChunks_ is a peer-controlled wire
    // input that would corrupt the watermark and risk a false DONE_OK on
    // a subsequent onEndAck. PURE layer rejects rather than relying on
    // the M3 wire parser to validate first.
    AstrOsBulkTransport::BulkSender s;
    ASSERT_TRUE(s.begin(7, /*totalChunks=*/10, 128, 8, 400, 3).valid);
    ASSERT_EQ(AstrOsBulkTransport::BeginAckResult::Decision::OK, s.onBeginAck(7).decision);

    // cumulativeSeq=10 is one past the last legal value (seqs are 0..9).
    auto r = s.onDataAck(7, /*cumulativeSeq=*/10);
    EXPECT_EQ(AstrOsBulkTransport::AckResult::Decision::OUT_OF_RANGE, r.decision);

    // UINT32_MAX is the wire-format upper bound; trips the same guard.
    auto r2 = s.onDataAck(7, UINT32_MAX);
    EXPECT_EQ(AstrOsBulkTransport::AckResult::Decision::OUT_OF_RANGE, r2.decision);
}

TEST(BulkTransport, BulkSenderOnDataNakRejectsOutOfRangeNextExpectedSeq)
{
    // Defensive: nextExpectedSeq >= totalChunks_ would advance nextSeqToSend_
    // past the transfer, causing the sender to silently skip chunks via the
    // ALL_SENT path. PURE layer rejects.
    AstrOsBulkTransport::BulkSender s;
    ASSERT_TRUE(s.begin(7, /*totalChunks=*/10, 128, 8, 400, 3).valid);
    ASSERT_EQ(AstrOsBulkTransport::BeginAckResult::Decision::OK, s.onBeginAck(7).decision);

    auto r = s.onDataNak(7, /*nextExpectedSeq=*/10, AstrOsBulkTransport::NakReason::CRC);
    EXPECT_EQ(AstrOsBulkTransport::NakResult::Decision::OUT_OF_RANGE, r.decision);

    auto r2 = s.onDataNak(7, UINT32_MAX, AstrOsBulkTransport::NakReason::CRC);
    EXPECT_EQ(AstrOsBulkTransport::NakResult::Decision::OUT_OF_RANGE, r2.decision);
}

TEST(BulkTransport, BulkSenderOnEndAckRejectsPrematureCall)
{
    // The implicit AWAITING_END_ACK predicate requires all chunks sent AND
    // all confirmed before onEndAck is valid. Calling onEndAck after just
    // begin + onBeginAck (zero sends, zero ACKs) must return PREMATURE.
    // windowSize=16 (MAX_WINDOW_SIZE) so all 10 chunks fit without WINDOW_FULL.
    AstrOsBulkTransport::BulkSender s;
    ASSERT_TRUE(s.begin(7, /*totalChunks=*/10, 128, /*windowSize=*/16, 400, 3).valid);
    ASSERT_EQ(AstrOsBulkTransport::BeginAckResult::Decision::OK, s.onBeginAck(7).decision);

    // Zero chunks sent; predicate fails.
    auto r1 = s.onEndAck(7, OtaEndStatus::OK);
    EXPECT_EQ(AstrOsBulkTransport::EndAckResult::Decision::PREMATURE, r1.decision);
    EXPECT_EQ(AstrOsBulkTransport::BulkSender::Status::STREAMING, s.status());

    // Send some but not all; predicate still fails.
    for (uint32_t i = 0; i < 5; i++)
    {
        auto sr = s.nextChunkToSend(1000);
        ASSERT_EQ(AstrOsBulkTransport::SendResult::Decision::SEND, sr.decision);
    }
    auto r2 = s.onEndAck(7, OtaEndStatus::OK);
    EXPECT_EQ(AstrOsBulkTransport::EndAckResult::Decision::PREMATURE, r2.decision);

    // Send all but ACK only some; predicate still fails (not all confirmed).
    for (uint32_t i = 5; i < 10; i++)
    {
        auto sr = s.nextChunkToSend(1000);
        ASSERT_EQ(AstrOsBulkTransport::SendResult::Decision::SEND, sr.decision);
    }
    ASSERT_EQ(AstrOsBulkTransport::AckResult::Decision::OK, s.onDataAck(7, 4).decision);
    auto r3 = s.onEndAck(7, OtaEndStatus::OK);
    EXPECT_EQ(AstrOsBulkTransport::EndAckResult::Decision::PREMATURE, r3.decision);

    // Finally confirm everything — onEndAck now succeeds.
    ASSERT_EQ(AstrOsBulkTransport::AckResult::Decision::OK, s.onDataAck(7, 9).decision);
    auto r4 = s.onEndAck(7, OtaEndStatus::OK);
    EXPECT_EQ(AstrOsBulkTransport::EndAckResult::Decision::DONE_OK, r4.decision);
}

TEST(BulkTransport, BulkSenderEndToEndHappyPath)
{
    // Drives the full state machine through a small transfer with the
    // exact API call pattern M3's OtaForwarder will use.
    // windowSize=8 > totalChunks=4 so all 4 chunks fit in the window
    // and drainSends returns ALL_SENT after emitting all of them.
    AstrOsBulkTransport::BulkSender s;
    ASSERT_TRUE(s.begin(/*xferId=*/42, /*totalChunks=*/4, 128, /*windowSize=*/8, 400, 3).valid);
    ASSERT_EQ(AstrOsBulkTransport::BulkSender::Status::AWAITING_BEGIN_ACK, s.status());

    ASSERT_EQ(AstrOsBulkTransport::BeginAckResult::Decision::OK, s.onBeginAck(42).decision);
    ASSERT_EQ(AstrOsBulkTransport::BulkSender::Status::STREAMING, s.status());

    // Pump nextChunkToSend — all 4 chunks fit in the window before ALL_SENT.
    std::vector<uint32_t> sent;
    auto term = drainSends(s, 1000, sent);
    EXPECT_EQ(AstrOsBulkTransport::SendResult::Decision::ALL_SENT, term);
    ASSERT_EQ(4u, sent.size()); // seqs 0..3 emitted

    // Receiver cumulatively ACKs all 4 (seqs 0..3).
    auto ackR = s.onDataAck(42, /*cumulativeSeq=*/3);
    EXPECT_EQ(AstrOsBulkTransport::AckResult::Decision::OK, ackR.decision);
    EXPECT_EQ(4u, ackR.newlyConfirmedCount);

    // nextChunkToSend still reports ALL_SENT — nothing left to emit.
    auto sendR = s.nextChunkToSend(1100);
    EXPECT_EQ(AstrOsBulkTransport::SendResult::Decision::ALL_SENT, sendR.decision);

    // Send OTA_END, receiver replies OK.
    auto endR = s.onEndAck(42, OtaEndStatus::OK);
    EXPECT_EQ(AstrOsBulkTransport::EndAckResult::Decision::DONE_OK, endR.decision);
    EXPECT_EQ(AstrOsBulkTransport::BulkSender::Status::DONE_OK, s.status());
}
