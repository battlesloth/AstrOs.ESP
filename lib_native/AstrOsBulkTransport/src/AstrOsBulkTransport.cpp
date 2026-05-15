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
        result.reason = NakReason::OUT_OF_ORDER;
        // highestContiguousSeq / nextExpectedSeq / windowRemaining default to 0 for the not-active path.

        if (!active_ || xferId != xferId_ || seq != nextSeq_)
        {
            // Subsequent tasks: explicit handling for CRC / SIZE / etc. For
            // now, anything other than a matching in-order chunk on an
            // active transfer NAKs as OUT_OF_ORDER.
            return result;
        }

        // Happy path: ACK and advance.
        // CRC + SIZE validation come in Task 5/6; for now we trust the inputs
        // to keep this task's diff focused on begin/reset/state.
        (void)crc16;
        (void)payloadLen;
        result.decision = Decision::ACK;
        result.reason = NakReason::NONE;
        result.highestContiguousSeq = seq;
        result.nextExpectedSeq = seq + 1;
        result.windowRemaining = windowSize_;
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
