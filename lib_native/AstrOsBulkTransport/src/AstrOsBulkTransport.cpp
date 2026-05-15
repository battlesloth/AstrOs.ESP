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

    // CRC-16/CCITT-FALSE. Bit-by-bit reference implementation:
    //   poly = 0x1021, init = 0xFFFF, refIn = false, refOut = false, xorOut = 0.
    // Table-based variants would be faster but the FW_CHUNK rate is bounded
    // by the 115200-baud serial link (~11 KB/s sustained, so each ~4 KB
    // chunk's CRC completes in well under a millisecond). No table needed.
    uint16_t crc16_ccitt_false(const uint8_t *data, size_t len)
    {
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

    void BulkReceiver::begin(uint8_t xferId, uint32_t totalSize, uint32_t totalChunks, uint16_t chunkSize,
                             uint8_t windowSize)
    {
        // Reject protocol-illegal parameters by leaving the receiver inactive.
        // A zero chunkSize would make every chunk NAK with SIZE (because
        // expectedLen would be 0 for any non-final seq) and would risk
        // confusing the SIZE math; zero totalChunks would let onEnd return
        // OK without ever receiving a chunk. The MIXED caller's
        // FW_TRANSFER_BEGIN parser should catch these earlier, but the
        // guard here keeps the state machine in a well-defined state.
        if (chunkSize == 0 || totalChunks == 0)
        {
            reset();
            return;
        }

        xferId_ = xferId;
        nextSeq_ = 0;
        totalSize_ = totalSize;
        totalChunks_ = totalChunks;
        chunkSize_ = chunkSize;
        windowSize_ = windowSize;
        active_ = true;
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
        ChunkResult result{};
        result.decision = Decision::NAK;
        // Reported window: windowSize_ on every active-state reply (ACK or
        // NAK) since the receiver tracks no in-flight state. 0 when the
        // receiver is not active — Phase 3 dispatches on this sentinel.
        result.windowRemaining = active_ ? windowSize_ : 0;

        // Structural rejections first: not-active, wrong xferId, out-of-seq.
        // All collapse to OUT_OF_ORDER per the wire-level reason-code set.
        if (!active_ || xferId != xferId_ || seq != nextSeq_)
        {
            result.reason = NakReason::OUT_OF_ORDER;
            // highestContiguousSeq / nextExpectedSeq default to 0 on the
            // not-active path; on an active mismatch they should reflect
            // the receiver's actual state so the sender can resync.
            if (active_)
            {
                result.highestContiguousSeq = (nextSeq_ == 0) ? 0 : (nextSeq_ - 1);
                result.nextExpectedSeq = nextSeq_;
            }
            return result;
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
            result.reason = NakReason::SIZE;
            result.highestContiguousSeq = (nextSeq_ == 0) ? 0 : (nextSeq_ - 1);
            result.nextExpectedSeq = nextSeq_;
            return result;
        }

        // CRC: recompute over the payload and compare.
        uint16_t computed = crc16_ccitt_false(payload, payloadLen);
        if (computed != crc16)
        {
            result.reason = NakReason::CRC;
            result.highestContiguousSeq = (nextSeq_ == 0) ? 0 : (nextSeq_ - 1);
            result.nextExpectedSeq = nextSeq_;
            return result;
        }

        // ACK and advance.
        result.decision = Decision::ACK;
        result.reason = NakReason::NONE;
        result.highestContiguousSeq = seq;
        result.nextExpectedSeq = seq + 1;
        result.payload = payload;
        result.payloadLen = payloadLen;
        nextSeq_++;
        return result;
    }

    EndResult BulkReceiver::onEnd(uint8_t xferId, uint32_t totalChunksSent)
    {
        EndResult result{};
        result.status = EndResult::Status::IO_ERROR;

        if (!active_ || xferId != xferId_)
        {
            return result;
        }
        if (totalChunksSent != totalChunks_)
        {
            return result;
        }
        if (nextSeq_ != totalChunks_)
        {
            return result;
        }

        result.status = EndResult::Status::OK;
        return result;
    }
} // namespace AstrOsBulkTransport
