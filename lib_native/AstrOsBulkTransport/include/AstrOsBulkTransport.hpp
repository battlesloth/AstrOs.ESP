#pragma once

#include <cstddef>
#include <cstdint>

namespace AstrOsBulkTransport
{
    // Single-frame CRC-16/CCITT-FALSE. Poly 0x1021, init 0xFFFF, no input
    // reflection, no output reflection, no XOR-out. Canonical check value:
    // crc16_ccitt_false("123456789", 9) == 0x29B1.
    //
    // Precondition: `data` must be non-null whenever `len > 0`. The
    // (anything, 0) case is well-defined and returns the init value
    // 0xFFFFu. Calling with (nullptr, len > 0) aborts via std::abort()
    // unconditionally (NDEBUG-safe — does not rely on assert remaining
    // compiled in). On native test builds the assert that fires first
    // gives the file:line + message; on ESP target abort() panics + resets.
    // Either way, deterministic loud failure, never UB.
    //
    // NOTE on ESP-IDF interop: this is NOT byte-identical to esp_crc16_le.
    // esp_crc16_le is CRC-16/CCITT (reflected, init=0) — a different
    // algorithm. The matching ESP-IDF variant is the big-endian one with a
    // tilde wrapper: `~esp_rom_crc16_be((uint16_t)~0xFFFF, buf, len)`. Phase
    // 3 should call this PURE function directly rather than wiring up an
    // ESP-IDF helper.
    uint16_t crc16_ccitt_false(const uint8_t *data, size_t len);

    enum class Decision : uint8_t
    {
        ACK,
        NAK
    };

    // Internal NAK reasons. Values `CRC` through `FLASH_FULL` map directly
    // to the wire-level FW_CHUNK_NAK reason codes (CRC | SIZE | OUT_OF_ORDER
    // | FLASH_FULL); the explicit numeric values are wire-stable and must
    // not be reordered. `NONE` is used only on the ACK path and has no wire
    // representation. `WRONG_TRANSFER_ID` and `NOT_ACTIVE` are NOT distinct
    // enumerators — those structural rejections all collapse to OUT_OF_ORDER,
    // matching the wire protocol's reason-code set.
    enum class NakReason : uint8_t
    {
        NONE = 0,
        CRC = 1,
        SIZE = 2,
        OUT_OF_ORDER = 3,
        FLASH_FULL = 4
    };

    // Result of `BulkReceiver::onChunk`. Carries both ACK and NAK information
    // in a single struct.
    //
    // Always populated:
    //   - `decision`         — ACK or NAK
    //   - `highestContiguousSeq` — last seq committed (0 if none committed yet)
    //   - `nextExpectedSeq`  — seq the sender should send next
    //   - `windowRemaining`  — see semantics below
    //
    // NAK path additionally populates:
    //   - `reason`           — CRC | SIZE | OUT_OF_ORDER | FLASH_FULL
    //                          Phase 3 maps this directly into FW_CHUNK_NAK's
    //                          reason-code field. highestContiguousSeq maps
    //                          to FW_CHUNK_NAK's `last-good-seq`.
    //                          IMPORTANT: on the first-chunk NAK (nextSeq_==0
    //                          before any ACK), highestContiguousSeq is also
    //                          0 — same as "seq 0 was committed." Phase 3
    //                          must use `nextExpectedSeq == 0` as the
    //                          sentinel for "nothing committed yet" rather
    //                          than relying on highestContiguousSeq alone.
    //
    // ACK path additionally populates:
    //   - `payload`/`payloadLen` — caller's input pointer + length pass
    //                          straight through, unmodified. BulkReceiver
    //                          does not own this memory and does not touch
    //                          the bytes. nullptr/0 on NAK.
    //
    // windowRemaining semantics:
    //   - On any reply from an active receiver (ACK, CRC NAK, SIZE NAK,
    //     OUT_OF_ORDER NAK on an active-state mismatch): equals the
    //     `windowSize` passed to `begin()`. The receiver tracks no
    //     in-flight state.
    //   - On a NAK from an inactive receiver (begin() not yet called or
    //     reset() was called): always 0. Phase 3 should treat
    //     `windowRemaining == 0` as the "not active, abort + restart the
    //     handshake" sentinel and any other value as "active-state
    //     mismatch; retransmit from `nextExpectedSeq`."
    // Construction discipline: the default constructor is private so
    // callers cannot aggregate-initialize impossible field combinations
    // (e.g. Decision::ACK with reason != NONE, or NAK with non-null
    // payload). Use the static factories: `ack`, `nakActive`,
    // `nakInactive`. Fields are public-readable AND `const` so that a
    // returned result cannot be mutated into an invalid combination
    // after the factory enforces consistency — test code keeps the
    // read-by-name idiom (`r.payload`, `r.reason`, etc.) but cannot
    // assign through it. Const fields disable copy/move assignment;
    // copy-construction (`auto r = factory()`) still works.
    struct ChunkResult
    {
        const Decision decision;
        const uint32_t highestContiguousSeq;
        const uint32_t nextExpectedSeq;
        const uint8_t windowRemaining;
        const NakReason reason;
        const uint8_t *const payload;
        const uint16_t payloadLen;

        // ACK: the chunk committed. `committedSeq` is the seq we just
        // committed; `nextExpectedSeq` is committedSeq + 1; both go on
        // the wire as FW_CHUNK_ACK fields.
        static ChunkResult ack(uint32_t committedSeq, uint32_t nextExpectedSeq, uint8_t windowSize,
                               const uint8_t *payload, uint16_t payloadLen)
        {
            return ChunkResult(Decision::ACK, committedSeq, nextExpectedSeq, windowSize, NakReason::NONE, payload,
                               payloadLen);
        }

        // Active NAK: receiver is in a live transfer but the chunk was
        // rejected (CRC | SIZE | OUT_OF_ORDER | FLASH_FULL). `windowSize`
        // here is the configured `windowSize` from begin() — used as the
        // "still active, retransmit from nextExpectedSeq" wire signal.
        static ChunkResult nakActive(NakReason reason, uint32_t highestContiguousSeq, uint32_t nextExpectedSeq,
                                     uint8_t windowSize)
        {
            return ChunkResult(Decision::NAK, highestContiguousSeq, nextExpectedSeq, windowSize, reason, nullptr, 0);
        }

        // Inactive NAK: chunk arrived while no transfer is live (begin()
        // not called or reset() was called). `windowRemaining = 0` is the
        // wire signal Phase 3 uses to say "abort + restart handshake."
        static ChunkResult nakInactive()
        {
            return ChunkResult(Decision::NAK, /*highestContiguousSeq=*/0, /*nextExpectedSeq=*/0,
                               /*windowRemaining=*/0, NakReason::OUT_OF_ORDER, /*payload=*/nullptr,
                               /*payloadLen=*/0);
        }

    private:
        // Private all-args constructor — the factories are the only legal
        // way to build a ChunkResult. Aggregate initialization is disabled
        // by the user-declared constructor, so external code can't bypass
        // the factory invariants by writing `ChunkResult{Decision::ACK, ...}`.
        ChunkResult(Decision d, uint32_t hcs, uint32_t nes, uint8_t wr, NakReason r, const uint8_t *p, uint16_t pl)
            : decision(d), highestContiguousSeq(hcs), nextExpectedSeq(nes), windowRemaining(wr), reason(r), payload(p),
              payloadLen(pl)
        {
        }
    };

    // [[nodiscard]] forces every caller (including tests) to acknowledge
    // the result — silently dropping `begin()` would leave the caller
    // unable to distinguish "transfer started" from "BEGIN was rejected,
    // receiver still inactive," which is precisely the silent-failure
    // case the PR-toolkit pass flagged as Critical.
    struct [[nodiscard]] BeginResult
    {
        // OK only when valid == true. Non-OK values describe why begin()
        // refused to activate the receiver — caller should reject the
        // wire-level FW_TRANSFER_BEGIN with a matching status code rather
        // than silently NAK every subsequent chunk.
        enum class Reason : uint8_t
        {
            OK = 0,
            ZERO_CHUNK_SIZE = 1,
            ZERO_TOTAL_CHUNKS = 2,
            ZERO_WINDOW_SIZE = 3,
            SIZE_INCONSISTENT = 4 // totalSize outside ((totalChunks - 1) * chunkSize, totalChunks * chunkSize]
        };
        bool valid = false;
        Reason reason = Reason::ZERO_CHUNK_SIZE;

        static BeginResult ok()
        {
            return {true, Reason::OK};
        }
        static BeginResult invalid(Reason r)
        {
            return {false, r};
        }
    };

    struct [[nodiscard]] EndResult
    {
        // HASH_MISMATCH is reserved for the Phase 3+ MIXED layer once it
        // has a hash context. The MIXED OtaReceiver introduced in Phase 3
        // sinks payload to /dev/null and never returns HASH_MISMATCH;
        // only the Phase 4 enhancement that adds the streaming SHA-256
        // context will ever produce it. This PURE state machine itself
        // never returns HASH_MISMATCH — onEnd only validates chunk counts
        // and returns OK on matching totals or IO_ERROR on mismatch.
        enum class Status : uint8_t
        {
            OK = 0,
            HASH_MISMATCH = 1,
            IO_ERROR = 2
        };

        // Populated when status != OK. NONE on the success path. Mirrors
        // the NakReason pattern: distinct reasons that all happen to map
        // to a single Status::IO_ERROR get surfaced so Phase 3 can log
        // them and operators can debug WHICH IO_ERROR they hit.
        enum class Reason : uint8_t
        {
            NONE = 0,
            NOT_ACTIVE = 1,            // begin() not called or reset() was called
            WRONG_XFER_ID = 2,         // xferId mismatch
            SENDER_TOTAL_MISMATCH = 3, // sender's totalChunksSent != totalChunks_
            RECEIVER_SHORT_COUNT = 4   // sender claims complete but nextSeq_ < totalChunks_
        };
        Status status = Status::IO_ERROR;
        Reason reason = Reason::NOT_ACTIVE;

        static EndResult ok()
        {
            return {Status::OK, Reason::NONE};
        }
        static EndResult ioError(Reason r)
        {
            return {Status::IO_ERROR, r};
        }
    };

    // Sequential chunk-receive state machine for the firmware OTA path.
    // The receiver commits chunks strictly in seq order — sliding window
    // is a sender optimization, not a reorder buffer.
    //
    // State machine contract:
    //   - A NAK leaves `nextSeq_` unchanged. The sender MUST retransmit
    //     the chunk identified by `cr.nextExpectedSeq` (same seq as the
    //     rejected one).
    //   - `windowRemaining` on every active-state reply (ACK or NAK)
    //     equals the `windowSize` passed to `begin()`. The "not active"
    //     case returns `windowRemaining = 0` as a distinct sentinel.
    //   - `onEnd` does NOT auto-reset the receiver on IO_ERROR — the
    //     caller can inspect `nextSeq_` (indirectly via subsequent
    //     `onChunk` calls' nextExpectedSeq field) before calling `reset()`.
    //   - Calling `begin()` on an already-active receiver is well-defined:
    //     it reinitializes the receiver as if `reset()` + `begin()` had
    //     been called. Useful when the caller decides to restart a
    //     transfer mid-stream.
    //   - `begin()` with `chunkSize == 0` or `totalChunks == 0` is treated
    //     as a protocol violation: the receiver is left inactive (callers
    //     get OUT_OF_ORDER NAKs from `onChunk`) AND `begin()` returns
    //     `BeginResult { valid=false, reason=ZERO_CHUNK_SIZE | ZERO_TOTAL_CHUNKS }`.
    //     The return value is `[[nodiscard]]`; the caller MUST check
    //     `result.valid` before reporting a successful FW_TRANSFER_BEGIN_ACK.
    //
    // Usage:
    //   BulkReceiver r;
    //   auto br = r.begin(xferId, totalSize, totalChunks, chunkSize, windowSize);
    //   if (!br.valid) {
    //       // Reject FW_TRANSFER_BEGIN with a status code derived from
    //       // br.reason (ZERO_CHUNK_SIZE | ZERO_TOTAL_CHUNKS |
    //       // ZERO_WINDOW_SIZE | SIZE_INCONSISTENT). Do NOT send a
    //       // success ACK — the receiver is inactive.
    //       return;
    //   }
    //   for each FW_CHUNK arrival:
    //       auto cr = r.onChunk(xferId, seq, payloadLen, crc16, payload);
    //       // On ACK: write cr.payload[0..cr.payloadLen-1] to flash,
    //       //         send FW_CHUNK_ACK with the seq/window fields.
    //       // On NAK: send FW_CHUNK_NAK(last-good-seq=highestContiguousSeq,
    //       //         reason=cr.reason). Sender retransmits cr.nextExpectedSeq.
    //   auto er = r.onEnd(xferId, totalChunksSent);
    //   // er.status == OK on success; on IO_ERROR, er.reason names the
    //   // specific cause (NOT_ACTIVE | WRONG_XFER_ID |
    //   // SENDER_TOTAL_MISMATCH | RECEIVER_SHORT_COUNT).
    //   r.reset();  // safe to call anytime; required before the next begin().
    class BulkReceiver
    {
    public:
        BeginResult begin(uint8_t xferId, uint32_t totalSize, uint32_t totalChunks, uint16_t chunkSize,
                          uint8_t windowSize);
        ChunkResult onChunk(uint8_t xferId, uint32_t seq, uint16_t payloadLen, uint16_t crc16, const uint8_t *payload);
        EndResult onEnd(uint8_t xferId, uint32_t totalChunksSent);
        void reset();

    private:
        // The wire field FW_CHUNK_NAK::last-good-seq. 0 is overloaded:
        // either "seq 0 was committed" or "nothing committed yet." Phase
        // 3 disambiguates via `nextExpectedSeq == 0` (only true when
        // nothing committed). Centralized here so the first-chunk-NAK
        // contract lives in one place rather than duplicated at every
        // NAK construction site.
        uint32_t lastGoodSeq() const
        {
            return (nextSeq_ == 0) ? 0u : (nextSeq_ - 1u);
        }

        uint8_t xferId_ = 0;
        uint32_t nextSeq_ = 0;
        uint32_t totalSize_ = 0;
        uint32_t totalChunks_ = 0;
        uint16_t chunkSize_ = 0;
        uint8_t windowSize_ = 0;
        bool active_ = false;
    };
} // namespace AstrOsBulkTransport
