#include "AstrOsBulkTransport.hpp"

namespace AstrOsBulkTransport
{
    // NakReason wire stability: values CRC..FLASH_FULL are serialized into
    // FW_CHUNK_NAK reason-code fields. Pin them at compile time so an
    // accidental reorder breaks the build, not the protocol.
    static_assert(static_cast<uint8_t>(NakReason::NONE) == 0);
    static_assert(static_cast<uint8_t>(NakReason::CRC) == 1);
    static_assert(static_cast<uint8_t>(NakReason::SIZE) == 2);
    static_assert(static_cast<uint8_t>(NakReason::OUT_OF_ORDER) == 3);
    static_assert(static_cast<uint8_t>(NakReason::FLASH_FULL) == 4);

    // API-stable enums consumed by Phase 3 dispatch via switch statements.
    // Not wire-serialized — these values won't end up on the bus — but a
    // future reorder would silently change which `case` fires for which
    // failure cause, with no compiler diagnostic to catch the bug. Pin
    // them here so the build breaks, not the dispatch.
    static_assert(static_cast<uint8_t>(BeginResult::Reason::OK) == 0);
    static_assert(static_cast<uint8_t>(BeginResult::Reason::ZERO_CHUNK_SIZE) == 1);
    static_assert(static_cast<uint8_t>(BeginResult::Reason::ZERO_TOTAL_CHUNKS) == 2);
    static_assert(static_cast<uint8_t>(BeginResult::Reason::ZERO_WINDOW_SIZE) == 3);
    static_assert(static_cast<uint8_t>(BeginResult::Reason::SIZE_INCONSISTENT) == 4);

    static_assert(static_cast<uint8_t>(EndResult::Status::OK) == 0);
    static_assert(static_cast<uint8_t>(EndResult::Status::HASH_MISMATCH) == 1);
    static_assert(static_cast<uint8_t>(EndResult::Status::IO_ERROR) == 2);

    static_assert(static_cast<uint8_t>(EndResult::Reason::NONE) == 0);
    static_assert(static_cast<uint8_t>(EndResult::Reason::NOT_ACTIVE) == 1);
    static_assert(static_cast<uint8_t>(EndResult::Reason::WRONG_XFER_ID) == 2);
    static_assert(static_cast<uint8_t>(EndResult::Reason::SENDER_TOTAL_MISMATCH) == 3);
    static_assert(static_cast<uint8_t>(EndResult::Reason::RECEIVER_SHORT_COUNT) == 4);

    // CRC-16/CCITT-FALSE. Bit-by-bit reference implementation:
    //   poly = 0x1021, init = 0xFFFF, refIn = false, refOut = false, xorOut = 0.
    // Table-based variants would be faster but the FW_CHUNK rate is bounded
    // by the 115200-baud serial link (~11 KB/s sustained, so each ~4 KB
    // chunk's CRC completes in well under a millisecond). No table needed.
    uint16_t crc16_ccitt_false(const uint8_t *data, size_t len)
    {
        // (anything, 0) is the empty-input case and returns the init value
        // 0xFFFFu. (nullptr, len > 0) is a caller programming error — the
        // earlier (data == nullptr || len == 0) form silently returned
        // 0xFFFFu in both cases, which made `payload=nullptr, payloadLen=K,
        // crc16=0xFFFFu` a valid-looking ACK candidate (matching the empty-
        // input CRC). Splitting the two cases makes the (nullptr, len>0)
        // path explicit. The state-machine caller `onChunk` is responsible
        // for rejecting nullptr-with-positive-len upstream (structural NAK);
        // this function falls through to the loop for nullptr+positive-len
        // and will UB on the deref, which is acceptable defense-in-depth
        // because the upstream guard runs first.
        if (len == 0)
        {
            return 0xFFFFu;
        }
        uint16_t crc = 0xFFFFu;
        for (size_t i = 0; i < len; i++)
        {
            crc ^= static_cast<uint16_t>(data[i]) << 8;
            for (int bit = 0; bit < 8; bit++)
            {
                if (crc & 0x8000u)
                {
                    crc = static_cast<uint16_t>((crc << 1) ^ 0x1021u);
                }
                else
                {
                    crc = static_cast<uint16_t>(crc << 1);
                }
            }
        }
        return crc;
    }

    BeginResult BulkReceiver::begin(uint8_t xferId, uint32_t totalSize, uint32_t totalChunks, uint16_t chunkSize,
                                    uint8_t windowSize)
    {
        // Reject protocol-illegal parameters by leaving the receiver inactive.
        // A zero chunkSize would make every chunk NAK with SIZE (because
        // expectedLen would be 0 for any non-final seq) and would risk
        // confusing the SIZE math; zero totalChunks would let onEnd return
        // OK without ever receiving a chunk. The MIXED caller's
        // FW_TRANSFER_BEGIN parser should catch these earlier, but the
        // guard here keeps the state machine in a well-defined state and
        // surfaces the specific rejection reason so Phase 3 can emit a
        // matching FW_TRANSFER_BEGIN_ACK status code.
        if (chunkSize == 0)
        {
            reset();
            return BeginResult::invalid(BeginResult::Reason::ZERO_CHUNK_SIZE);
        }
        if (totalChunks == 0)
        {
            reset();
            return BeginResult::invalid(BeginResult::Reason::ZERO_TOTAL_CHUNKS);
        }
        // A zero windowSize defeats the windowRemaining-as-not-active-sentinel
        // contract: every reply from an active receiver would report
        // windowRemaining=0 (the configured size), making it indistinguishable
        // from the inactive case (windowRemaining=0 sentinel). Phase 3's
        // "abort + restart handshake" dispatch would fire on every chunk.
        if (windowSize == 0)
        {
            reset();
            return BeginResult::invalid(BeginResult::Reason::ZERO_WINDOW_SIZE);
        }
        // totalSize must be in the half-open interval
        // ((totalChunks - 1) * chunkSize, totalChunks * chunkSize].
        // Outside this range, the SIZE math at onChunk lines 27/28 either
        // overflows or produces a meaningless expectedLen (underflow when
        // totalSize < committedBytes). Computed in uint64_t to avoid the
        // overflow itself happening in the check.
        const uint64_t maxBytes = static_cast<uint64_t>(totalChunks) * chunkSize;
        const uint64_t minBytes = (totalChunks == 1) ? 1u : (static_cast<uint64_t>(totalChunks - 1) * chunkSize) + 1u;
        if (totalSize == 0 || totalSize > maxBytes || totalSize < minBytes)
        {
            reset();
            return BeginResult::invalid(BeginResult::Reason::SIZE_INCONSISTENT);
        }

        xferId_ = xferId;
        nextSeq_ = 0;
        totalSize_ = totalSize;
        totalChunks_ = totalChunks;
        chunkSize_ = chunkSize;
        windowSize_ = windowSize;
        active_ = true;
        return BeginResult::ok();
    }

    void BulkReceiver::reset()
    {
        xferId_ = 0;
        nextSeq_ = 0;
        totalSize_ = 0;
        totalChunks_ = 0;
        chunkSize_ = 0;
        windowSize_ = 0;
        active_ = false;
    }

    ChunkResult BulkReceiver::onChunk(uint8_t xferId, uint32_t seq, uint16_t payloadLen, uint16_t crc16,
                                      const uint8_t *payload)
    {
        // Not-active: distinct from "active state mismatch" via the
        // windowRemaining=0 sentinel that ChunkResult::nakInactive
        // produces. Phase 3 dispatches differently on the two cases.
        if (!active_)
        {
            return ChunkResult::nakInactive();
        }

        // Structural rejections: wrong xferId, out-of-seq, seq outside the
        // chunk grid, or null payload with non-zero length. Range guard
        // prevents `seq * chunkSize_` from overflowing uint32_t in the SIZE
        // math below — in well-behaved code the `seq != nextSeq_` reject
        // catches this earlier (nextSeq_ is bounded by totalChunks_), but
        // defense-in-depth pins it as a structural property of onChunk
        // rather than an emergent one. The nullptr-payload check closes a
        // genuine collision the CRC-empty-buffer guard would otherwise
        // create: crc16_ccitt_false(nullptr, len) returns 0xFFFFu by
        // contract, so a caller passing (payload=nullptr, payloadLen=K,
        // crc16=0xFFFF) would pass SIZE (K matches expectedLen) AND CRC
        // (computed == claimed), producing an ACK with payload=nullptr
        // that Phase 3 would deref. All four cases collapse to
        // OUT_OF_ORDER per the wire-level reason-code set.
        if (xferId != xferId_ || seq != nextSeq_ || seq >= totalChunks_ || (payloadLen > 0 && payload == nullptr))
        {
            return ChunkResult::nakActive(NakReason::OUT_OF_ORDER, lastGoodSeq(), nextSeq_, windowSize_);
        }

        // SIZE: compute the expected length for THIS seq. All chunks are
        // chunkSize_ bytes except possibly the last one (totalSize_ may not
        // be a clean multiple of chunkSize_).
        uint32_t expectedLen = chunkSize_;
        uint32_t committedBytes = static_cast<uint32_t>(seq) * chunkSize_;
        if (committedBytes + chunkSize_ > totalSize_)
        {
            expectedLen = totalSize_ - committedBytes;
        }
        if (payloadLen != expectedLen)
        {
            return ChunkResult::nakActive(NakReason::SIZE, lastGoodSeq(), nextSeq_, windowSize_);
        }

        if (crc16_ccitt_false(payload, payloadLen) != crc16)
        {
            return ChunkResult::nakActive(NakReason::CRC, lastGoodSeq(), nextSeq_, windowSize_);
        }

        nextSeq_++;
        return ChunkResult::ack(seq, seq + 1, windowSize_, payload, payloadLen);
    }

    EndResult BulkReceiver::onEnd(uint8_t xferId, uint32_t totalChunksSent)
    {
        // Four distinct IO_ERROR causes — surface each with its own
        // Reason so Phase 3 can log them distinctly. The four conditions
        // intentionally check in order of caller-fault diagnosis: "is
        // the receiver even live?" → "are we talking about the same
        // transfer?" → "does the sender agree with us on the total?" →
        // "did we actually receive all the chunks?"
        if (!active_)
        {
            return EndResult::ioError(EndResult::Reason::NOT_ACTIVE);
        }
        if (xferId != xferId_)
        {
            return EndResult::ioError(EndResult::Reason::WRONG_XFER_ID);
        }
        if (totalChunksSent != totalChunks_)
        {
            return EndResult::ioError(EndResult::Reason::SENDER_TOTAL_MISMATCH);
        }
        if (nextSeq_ != totalChunks_)
        {
            return EndResult::ioError(EndResult::Reason::RECEIVER_SHORT_COUNT);
        }
        return EndResult::ok();
    }
} // namespace AstrOsBulkTransport
