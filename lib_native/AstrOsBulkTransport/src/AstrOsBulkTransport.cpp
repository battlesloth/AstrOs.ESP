#include "AstrOsBulkTransport.hpp"

namespace AstrOsBulkTransport
{
    // CRC-16/CCITT-FALSE. Bit-by-bit reference implementation:
    //   poly = 0x1021, init = 0xFFFF, refIn = false, refOut = false, xorOut = 0.
    // Table-based variants would be faster but the volume here (one chunk =
    // ~4 KB per frame, ~5 MB/s peak throughput) doesn't justify the static
    // table cost in a PURE lib that has no on-device performance pressure.
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
        // Reported window: always windowSize_ on every reply. The receiver
        // tracks no in-flight state.
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
        // Stub for Task 4: prevents linker error. Real implementation lands in Task 7.
        (void)xferId;
        (void)totalChunksSent;
        return EndResult{};
    }
} // namespace AstrOsBulkTransport
