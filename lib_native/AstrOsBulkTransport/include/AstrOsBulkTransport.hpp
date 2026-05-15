#pragma once

#include <cstddef>
#include <cstdint>

namespace AstrOsBulkTransport
{
    // Single-frame CRC-16/CCITT-FALSE. Poly 0x1021, init 0xFFFF, no input
    // reflection, no output reflection, no XOR-out. Byte-identical to
    // ESP-IDF's esp_crc16_le.
    uint16_t crc16_ccitt_false(const uint8_t *data, size_t len);

    enum class Decision
    {
        ACK,
        NAK
    };

    // Internal NAK reasons map 1:1 to the wire-level FW_CHUNK_NAK reason
    // codes (CRC | SIZE | OUT_OF_ORDER | FLASH_FULL). `WRONG_TRANSFER_ID`
    // and `NOT_ACTIVE` are NOT distinct enum entries — those structural
    // rejections all map to OUT_OF_ORDER, matching the wire protocol.
    enum class NakReason : uint8_t
    {
        NONE = 0,
        CRC = 1,
        SIZE = 2,
        OUT_OF_ORDER = 3,
        FLASH_FULL = 4
    };

    struct ChunkResult
    {
        Decision decision = Decision::NAK;
        uint32_t highestContiguousSeq = 0;
        uint32_t nextExpectedSeq = 0;
        uint8_t windowRemaining = 0;
        NakReason reason = NakReason::NONE;
        // On ACK only: the caller's input pointer + length pass straight
        // through, unmodified. BulkReceiver does not own this memory and
        // does not touch the bytes. nullptr/0 on NAK.
        const uint8_t *payload = nullptr;
        uint16_t payloadLen = 0;
    };

    struct EndResult
    {
        // HASH_MISMATCH is reserved for the MIXED-layer caller (Phase 3
        // OtaReceiver) that owns the streaming SHA-256 context. This
        // PURE state machine only knows about chunk counts; it returns
        // OK on matching totals and IO_ERROR on mismatch.
        enum class Status
        {
            OK,
            HASH_MISMATCH,
            IO_ERROR
        };
        Status status = Status::IO_ERROR;
    };

    // Sequential chunk-receive state machine for the firmware OTA path.
    // The receiver commits chunks strictly in seq order — sliding window
    // is a sender optimization, not a reorder buffer. After every ACK
    // the receiver reports `windowRemaining = windowSize_` because it
    // tracks no in-flight state (each ACK'd chunk is already consumed
    // by the caller).
    //
    // Usage:
    //   BulkReceiver r;
    //   r.begin(xferId, totalSize, totalChunks, chunkSize, windowSize);
    //   for each FW_CHUNK arrival:
    //       auto cr = r.onChunk(xferId, seq, len, crc16, payload);
    //       // caller acts on cr.decision (write bytes / send ACK / send NAK)
    //   auto er = r.onEnd(xferId, totalChunksSent);
    //   r.reset();  // ready for next transfer; safe to call anytime.
    class BulkReceiver
    {
    public:
        void begin(uint8_t xferId, uint32_t totalSize, uint32_t totalChunks, uint16_t chunkSize, uint8_t windowSize);
        ChunkResult onChunk(uint8_t xferId, uint32_t seq, uint16_t payloadLen, uint16_t crc16, const uint8_t *payload);
        EndResult onEnd(uint8_t xferId, uint32_t totalChunksSent);
        void reset();

    private:
        uint8_t xferId_ = 0;
        uint32_t nextSeq_ = 0;
        uint32_t totalSize_ = 0;
        uint32_t totalChunks_ = 0;
        uint16_t chunkSize_ = 0;
        uint8_t windowSize_ = 0;
        bool active_ = false;
    };
} // namespace AstrOsBulkTransport
