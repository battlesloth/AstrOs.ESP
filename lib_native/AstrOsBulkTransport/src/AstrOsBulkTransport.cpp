#include "AstrOsBulkTransport.hpp"

#include <cassert>
#include <cstdlib>
#include <type_traits>

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

    // ChunkResult immutability: every public field must be const so a
    // factory-produced result can be inspected by callers but not mutated
    // into an invalid combination after construction. Implicitly disables
    // copy/move assignment (`r1 = r2` won't compile) while preserving
    // copy-construction (`auto r1 = factory()` still works) and prvalue
    // returns from the factories. Pinned at compile time so a future
    // refactor that drops `const` from any field breaks the build.
    static_assert(std::is_const_v<decltype(ChunkResult::decision)>);
    static_assert(std::is_const_v<decltype(ChunkResult::highestContiguousSeq)>);
    static_assert(std::is_const_v<decltype(ChunkResult::nextExpectedSeq)>);
    static_assert(std::is_const_v<decltype(ChunkResult::windowRemaining)>);
    static_assert(std::is_const_v<decltype(ChunkResult::reason)>);
    static_assert(std::is_const_v<decltype(ChunkResult::payload)>);
    static_assert(std::is_const_v<decltype(ChunkResult::payloadLen)>);
    static_assert(!std::is_copy_assignable_v<ChunkResult>);
    static_assert(std::is_copy_constructible_v<ChunkResult>);

    // CRC-16/CCITT-FALSE. Bit-by-bit reference implementation:
    //   poly = 0x1021, init = 0xFFFF, refIn = false, refOut = false, xorOut = 0.
    // Table-based variants would be faster but the FW_CHUNK rate is bounded
    // by the 115200-baud serial link (~11 KB/s sustained, so each ~4 KB
    // chunk's CRC completes in well under a millisecond). No table needed.
    uint16_t crc16_ccitt_false(const uint8_t *data, size_t len)
    {
        // Precondition: `data` must be non-null whenever `len > 0`.
        //   - (anything, 0) is the legitimate empty-input case and returns
        //     the init value 0xFFFFu.
        //   - (nullptr, len > 0) is a caller programming error. The check
        //     below uses an unconditional abort path (NOT bare assert) so
        //     the precondition holds in NDEBUG/release builds too — assert()
        //     alone would compile out and re-introduce the UB on deref.
        //     The assert() is kept for the failure message (file:line +
        //     expression text via __assert_func); abort() is the actual
        //     terminator that runs regardless of NDEBUG.
        //
        // History: the earlier (data == nullptr || len == 0) form silently
        // returned 0xFFFFu in BOTH cases, which made a caller passing
        // `payload=nullptr, payloadLen=K, crc16=0xFFFFu` look like a valid
        // ACK candidate (computed CRC matched the claimed CRC by
        // coincidence). BulkReceiver::onChunk has its own structural guard
        // for nullptr-with-positive-len, but this function is a public
        // API — any future caller (including code outside BulkReceiver)
        // gets the same deterministic failure mode.
        if (data == nullptr && len > 0)
        {
            assert(false && "crc16_ccitt_false: data is nullptr but len > 0");
            std::abort();
        }
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
        // Outside this range, onChunk's `committedBytes` / `expectedLen`
        // SIZE math either overflows (committedBytes + chunkSize_ wraps
        // when totalSize is near UINT32_MAX) or underflows
        // (totalSize_ - committedBytes when totalSize < committedBytes),
        // producing meaningless expectedLen values and confusing SIZE
        // NAKs. Computed in uint64_t to avoid the overflow itself
        // happening in the check.
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

    bool shouldTeardownOnEndResult(const EndResult &er)
    {
        if (er.status == EndResult::Status::OK)
        {
            return true;
        }
        switch (er.reason)
        {
        case EndResult::Reason::SENDER_TOTAL_MISMATCH:
        case EndResult::Reason::RECEIVER_SHORT_COUNT:
            return true;
        case EndResult::Reason::NONE:
        case EndResult::Reason::NOT_ACTIVE:
        case EndResult::Reason::WRONG_XFER_ID:
            return false;
        }
        return false; // unknown reason — conservative: preserve state
    }

    // BulkSender::Status integer values are API-stable and consumed by
    // switch statements in M3's OtaForwarder. Pin them so a future
    // reorder breaks the build, not the dispatch.
    static_assert(static_cast<uint8_t>(BulkSender::Status::IDLE) == 0);
    static_assert(static_cast<uint8_t>(BulkSender::Status::AWAITING_BEGIN_ACK) == 1);
    static_assert(static_cast<uint8_t>(BulkSender::Status::STREAMING) == 2);
    static_assert(static_cast<uint8_t>(BulkSender::Status::DONE_OK) == 3);
    static_assert(static_cast<uint8_t>(BulkSender::Status::ABANDONED) == 4);

    static_assert(static_cast<uint8_t>(BeginSenderResult::Reason::OK) == 0);
    static_assert(static_cast<uint8_t>(BeginSenderResult::Reason::ZERO_TOTAL_CHUNKS) == 1);
    static_assert(static_cast<uint8_t>(BeginSenderResult::Reason::ZERO_CHUNK_SIZE) == 2);
    static_assert(static_cast<uint8_t>(BeginSenderResult::Reason::ZERO_WINDOW_SIZE) == 3);
    static_assert(static_cast<uint8_t>(BeginSenderResult::Reason::ZERO_ACK_TIMEOUT) == 4);
    static_assert(static_cast<uint8_t>(BeginSenderResult::Reason::ZERO_MAX_RETRIES) == 5);
    static_assert(static_cast<uint8_t>(BeginSenderResult::Reason::WINDOW_TOO_LARGE) == 6);

    static_assert(static_cast<uint8_t>(BeginAckResult::Decision::OK) == 0);
    static_assert(static_cast<uint8_t>(BeginAckResult::Decision::WRONG_XFER_ID) == 1);
    static_assert(static_cast<uint8_t>(BeginAckResult::Decision::NOT_AWAITING_BEGIN_ACK) == 2);

    BeginSenderResult BulkSender::begin(uint8_t xferId, uint32_t totalChunks, uint16_t chunkSize, uint8_t windowSize,
                                        uint32_t ackTimeoutMs, uint8_t maxRetries)
    {
        // Reject protocol-illegal parameters in the order the contract
        // freezes them. Each rejection leaves the sender in IDLE so a
        // subsequent corrected begin() works cleanly. Order matters: a
        // future reorder of these guards would change which Reason a
        // caller sees for multi-fault input.
        if (totalChunks == 0)
        {
            reset();
            return BeginSenderResult::invalid(BeginSenderResult::Reason::ZERO_TOTAL_CHUNKS);
        }
        if (chunkSize == 0)
        {
            reset();
            return BeginSenderResult::invalid(BeginSenderResult::Reason::ZERO_CHUNK_SIZE);
        }
        if (windowSize == 0)
        {
            reset();
            return BeginSenderResult::invalid(BeginSenderResult::Reason::ZERO_WINDOW_SIZE);
        }
        if (windowSize > MAX_WINDOW_SIZE)
        {
            reset();
            return BeginSenderResult::invalid(BeginSenderResult::Reason::WINDOW_TOO_LARGE);
        }
        if (ackTimeoutMs == 0)
        {
            reset();
            return BeginSenderResult::invalid(BeginSenderResult::Reason::ZERO_ACK_TIMEOUT);
        }
        if (maxRetries == 0)
        {
            reset();
            return BeginSenderResult::invalid(BeginSenderResult::Reason::ZERO_MAX_RETRIES);
        }

        xferId_ = xferId;
        totalChunks_ = totalChunks;
        chunkSize_ = chunkSize;
        windowSize_ = windowSize;
        ackTimeoutMs_ = ackTimeoutMs;
        maxRetries_ = maxRetries;
        nextSeqToSend_ = 0;
        highestConfirmedSeq_ = 0;
        anyConfirmed_ = false;
        for (auto &e : inFlight_)
        {
            e = InFlightEntry{};
        }
        status_ = Status::AWAITING_BEGIN_ACK;
        return BeginSenderResult::ok();
    }

    BeginAckResult BulkSender::onBeginAck(uint8_t xferId)
    {
        if (status_ != Status::AWAITING_BEGIN_ACK)
        {
            return BeginAckResult::notAwaiting();
        }
        if (xferId != xferId_)
        {
            return BeginAckResult::wrongXferId();
        }
        status_ = Status::STREAMING;
        return BeginAckResult::ok();
    }

    void BulkSender::reset()
    {
        xferId_ = 0;
        totalChunks_ = 0;
        chunkSize_ = 0;
        windowSize_ = 0;
        ackTimeoutMs_ = 0;
        maxRetries_ = 0;
        nextSeqToSend_ = 0;
        highestConfirmedSeq_ = 0;
        anyConfirmed_ = false;
        for (auto &e : inFlight_)
        {
            e = InFlightEntry{};
        }
        status_ = Status::IDLE;
    }
} // namespace AstrOsBulkTransport
