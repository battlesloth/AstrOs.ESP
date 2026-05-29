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

    static_assert(static_cast<uint8_t>(SendResult::Decision::SEND) == 0);
    static_assert(static_cast<uint8_t>(SendResult::Decision::WINDOW_FULL) == 1);
    static_assert(static_cast<uint8_t>(SendResult::Decision::ALL_SENT) == 2);
    static_assert(static_cast<uint8_t>(SendResult::Decision::NOT_STREAMING) == 3);

    // SendResult immutability: every public field must be const so a
    // factory-produced result can't be mutated into an invalid combo
    // (WINDOW_FULL with a meaningful seq, etc.). Mirrors ChunkResult's
    // discipline.
    static_assert(std::is_const_v<decltype(SendResult::decision)>);
    static_assert(std::is_const_v<decltype(SendResult::seq)>);
    static_assert(!std::is_copy_assignable_v<SendResult>);
    static_assert(std::is_copy_constructible_v<SendResult>);

    static_assert(static_cast<uint8_t>(AckResult::Decision::OK) == 0);
    static_assert(static_cast<uint8_t>(AckResult::Decision::WRONG_XFER_ID) == 1);
    static_assert(static_cast<uint8_t>(AckResult::Decision::NOT_STREAMING) == 2);
    static_assert(static_cast<uint8_t>(AckResult::Decision::STALE) == 3);
    static_assert(static_cast<uint8_t>(AckResult::Decision::OUT_OF_RANGE) == 4);

    static_assert(static_cast<uint8_t>(NakResult::Decision::OK) == 0);
    static_assert(static_cast<uint8_t>(NakResult::Decision::WRONG_XFER_ID) == 1);
    static_assert(static_cast<uint8_t>(NakResult::Decision::NOT_STREAMING) == 2);
    static_assert(static_cast<uint8_t>(NakResult::Decision::OUT_OF_RANGE) == 3);

    static_assert(std::is_const_v<decltype(AckResult::decision)>);
    static_assert(std::is_const_v<decltype(AckResult::newlyConfirmedCount)>);
    static_assert(!std::is_copy_assignable_v<AckResult>);

    static_assert(std::is_const_v<decltype(NakResult::decision)>);
    static_assert(std::is_const_v<decltype(NakResult::nextSeqToResend)>);
    static_assert(!std::is_copy_assignable_v<NakResult>);

    static_assert(static_cast<uint8_t>(EndAckResult::Decision::DONE_OK) == 0);
    static_assert(static_cast<uint8_t>(EndAckResult::Decision::ABANDONED) == 1);
    static_assert(static_cast<uint8_t>(EndAckResult::Decision::WRONG_XFER_ID) == 2);
    static_assert(static_cast<uint8_t>(EndAckResult::Decision::NOT_STREAMING) == 3);
    static_assert(static_cast<uint8_t>(EndAckResult::Decision::PREMATURE) == 4);

    static_assert(std::is_const_v<decltype(EndAckResult::decision)>);
    static_assert(!std::is_copy_assignable_v<EndAckResult>);

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
        highWaterSentSeq_ = 0;
        highestConfirmedSeq_ = 0;
        anyConfirmed_ = false;
        inFlight_.fill(InFlightEntry{});
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

    SendResult BulkSender::nextChunkToSend(uint64_t nowMs)
    {
        if (status_ != Status::STREAMING)
        {
            return SendResult::notStreaming();
        }

        // ALL_SENT check first: if every chunk has been launched into
        // the in-flight table (or already evicted by ACK), there's
        // nothing left to claim. The MIXED caller treats ALL_SENT
        // (plus an empty in-flight table) as the signal to send OTA_END.
        if (nextSeqToSend_ >= totalChunks_)
        {
            return SendResult::allSent();
        }

        // Count occupied slots. WINDOW_FULL fires when we've already
        // launched `windowSize_` chunks that haven't been acked yet.
        // O(MAX_WINDOW_SIZE) linear scan; trivial at 16 entries.
        uint8_t occupied = 0;
        for (const auto &e : inFlight_)
        {
            if (e.occupied)
            {
                occupied++;
            }
        }
        if (occupied >= windowSize_)
        {
            return SendResult::windowFull();
        }

        // Claim the first unoccupied slot. The slot index is internal;
        // callers identify chunks by seq, not slot.
        for (auto &e : inFlight_)
        {
            if (!e.occupied)
            {
                e.seq = nextSeqToSend_;
                e.sendTimestampMs = nowMs;
                e.retryCount = 0;
                e.occupied = true;
                const uint32_t emittedSeq = nextSeqToSend_;
                nextSeqToSend_++;
                if (nextSeqToSend_ > highWaterSentSeq_)
                {
                    highWaterSentSeq_ = nextSeqToSend_;
                }
                return SendResult::send(emittedSeq);
            }
        }

        // Unreachable: if `occupied < windowSize_ <= MAX_WINDOW_SIZE`,
        // at least one slot is free. The early-return above already
        // returned WINDOW_FULL in the full case.
        assert(false && "BulkSender::nextChunkToSend: in-flight table full despite occupied < windowSize_");
        std::abort();
    }

    AckResult BulkSender::onDataAck(uint8_t xferId, uint32_t cumulativeSeq)
    {
        if (status_ != Status::STREAMING)
        {
            return AckResult::notStreaming();
        }
        if (xferId != xferId_)
        {
            return AckResult::wrongXferId();
        }
        // Bounds check: cumulativeSeq must reference a seq we've actually sent.
        // Defends the PURE state machine against peer-controlled wire input;
        // the M3 wire parser can also gate this, but we don't want to rely on
        // it. Two bounds:
        //   - cumulativeSeq >= totalChunks_: the seq doesn't exist in this
        //     transfer at all. Without this guard, UINT32_MAX would wrap the
        //     newlyConfirmedCount arithmetic below (cumulativeSeq + 1 - prev)
        //     to zero; any cumulativeSeq in [totalChunks_, UINT32_MAX-1] would
        //     corrupt highestConfirmedSeq_ (causing later valid-range ACKs to
        //     be falsely STALE).
        //   - cumulativeSeq >= highWaterSentSeq_: the seq exists in the
        //     transfer but we haven't launched it yet (highWaterSentSeq_ is
        //     the max nextSeqToSend_ ever reached; it never rewinds on NAK).
        //     The receiver cannot legitimately ACK what we haven't sent.
        //     Without this guard, a peer ACK ahead of the sender (e.g.
        //     cumulativeSeq=15 when highWaterSentSeq_=8) would evict all real
        //     in-flight slots, advance the watermark past unsent seqs, and
        //     stall the transfer (later legitimate ACKs for seqs 0..7 would
        //     be falsely STALE; tick wouldn't time out evicted slots;
        //     nextChunkToSend would happily emit seqs the receiver thinks
        //     it's already received).
        if (cumulativeSeq >= totalChunks_ || cumulativeSeq >= highWaterSentSeq_)
        {
            return AckResult::outOfRange();
        }
        // Stale check: cumulativeSeq must strictly advance from the
        // previous high-water mark. The `anyConfirmed_` flag
        // disambiguates "no ACK yet seen" (any cumulativeSeq is fresh)
        // from "seq 0 already confirmed" (only cumulativeSeq > 0 is
        // fresh).
        if (anyConfirmed_ && cumulativeSeq <= highestConfirmedSeq_)
        {
            return AckResult::stale();
        }

        const uint32_t prev = anyConfirmed_ ? highestConfirmedSeq_ + 1 : 0;
        highestConfirmedSeq_ = cumulativeSeq;
        anyConfirmed_ = true;

        // Evict all in-flight slots whose seq <= cumulativeSeq.
        for (auto &e : inFlight_)
        {
            if (e.occupied && e.seq <= cumulativeSeq)
            {
                e = InFlightEntry{};
            }
        }

        const uint32_t newlyConfirmed = cumulativeSeq + 1 - prev;
        return AckResult::ok(newlyConfirmed);
    }

    NakResult BulkSender::onDataNak(uint8_t xferId, uint32_t nextExpectedSeq, NakReason /*reason*/)
    {
        if (status_ != Status::STREAMING)
        {
            return NakResult::notStreaming();
        }
        if (xferId != xferId_)
        {
            return NakResult::wrongXferId();
        }
        // Bounds check: nextExpectedSeq must reference a seq we've launched
        // (or one-past — the degenerate no-op case). Defends against
        // peer-controlled wire input. Two bounds:
        //   - nextExpectedSeq >= totalChunks_: the seq doesn't exist in this
        //     transfer. Without this guard, a peer NAK with nextExpectedSeq
        //     >= totalChunks_ would advance nextSeqToSend_ past totalChunks_,
        //     causing the sender to silently skip real chunks via the
        //     ALL_SENT path on the next nextChunkToSend.
        //   - nextExpectedSeq > nextSeqToSend_: the seq is within the
        //     transfer but ahead of where we've launched. NAK semantics are
        //     "rewind to here" — fast-forwarding is a wire-protocol
        //     violation that would skip chunks we haven't even sent. Equality
        //     (nextExpectedSeq == nextSeqToSend_) is the degenerate no-op
        //     where the receiver asks for what we'd send next anyway —
        //     accepted to be permissive on the boundary.
        // Note on asymmetry with onDataAck: NAK uses the *current* nextSeqToSend_
        // (which rewinds), not highWaterSentSeq_. NAK semantics are "rewind to
        // here, resume from here" — the receiver may legitimately reference any
        // seq up to where we'll send next, but never beyond it. A stale NAK from
        // before a rewind referencing a seq > current nextSeqToSend_ is no longer
        // applicable (the receiver's view of the wire was overtaken by our
        // rewind + retransmit). Rejecting it is correct.
        if (nextExpectedSeq >= totalChunks_ || nextExpectedSeq > nextSeqToSend_)
        {
            return NakResult::outOfRange();
        }

        // NAK semantics: nextExpectedSeq carries cumulative-ACK information —
        // the receiver implicitly confirms seqs 0..nextExpectedSeq-1. Advance
        // highestConfirmedSeq_ so a subsequent OTA_DATA_ACK at or below the
        // implied cumulative seq is correctly rejected as STALE rather than
        // being treated as new progress (which would corrupt the
        // newlyConfirmedCount arithmetic). The protection against tick
        // charging retries against already-accepted seqs comes from the
        // unconditional inFlight_.fill() reset below — not from this
        // watermark advance.
        //
        // nextExpectedSeq == 0 is the "receiver got nothing" case — no
        // implicit confirmation; leave the watermark untouched.
        if (nextExpectedSeq > 0)
        {
            const uint32_t impliedCumulative = nextExpectedSeq - 1;
            if (!anyConfirmed_ || impliedCumulative > highestConfirmedSeq_)
            {
                highestConfirmedSeq_ = impliedCumulative;
                anyConfirmed_ = true;
            }
        }

        // Rewind: future sends start from nextExpectedSeq. Evict ALL in-flight
        // slots — those below nextExpectedSeq are now implicitly confirmed
        // (handled above) and those at or above need retransmission via
        // subsequent nextChunkToSend calls. The MIXED layer doesn't need to
        // know which slot belongs to which seq.
        //
        // Reason is currently unused by the state machine (the wire layer
        // logs it in M3); it's in the signature so future reason-aware
        // retry policies don't break the API.
        nextSeqToSend_ = nextExpectedSeq;
        inFlight_.fill(InFlightEntry{});
        return NakResult::ok(nextExpectedSeq);
    }

    TickResult BulkSender::tick(uint64_t nowMs)
    {
        TickResult result{};
        if (status_ != Status::STREAMING)
        {
            return result;
        }

        for (auto &e : inFlight_)
        {
            if (!e.occupied)
            {
                continue;
            }
            // Timeout fired iff (nowMs - sendTimestampMs) >= ackTimeoutMs_.
            // Use subtraction in uint64_t so wrap-around isn't a concern at
            // typical millisecond scales; if a future caller passes nowMs <
            // sendTimestampMs (clock went backwards), the underflow wraps
            // to a huge value and the timeout looks "fired" — acceptable
            // failure mode (retransmits a slot that wasn't actually timed
            // out). The MIXED caller is responsible for monotonic time.
            if (nowMs - e.sendTimestampMs < ackTimeoutMs_)
            {
                continue;
            }

            // Retry count check: if incrementing would exceed maxRetries_,
            // abandon the whole transfer immediately. The wire-level
            // contract says "abort this padawan, move to next" — at the
            // PURE layer we just transition to ABANDONED.
            if (e.retryCount + 1 > maxRetries_)
            {
                status_ = Status::ABANDONED;
                result.abandon = true;
                result.count = 0;
                return result;
            }

            e.retryCount++;
            e.sendTimestampMs = nowMs;
            result.retransmitSeqs[result.count] = e.seq;
            result.count++;
        }
        return result;
    }

    EndAckResult BulkSender::onEndAck(uint8_t xferId, OtaEndStatus status)
    {
        if (status_ != Status::STREAMING)
        {
            return EndAckResult::notStreaming();
        }
        if (xferId != xferId_)
        {
            return EndAckResult::wrongXferId();
        }
        // Implicit AWAITING_END_ACK predicate: all chunks must be sent AND all
        // confirmed before onEndAck is valid. The class doc says AWAITING_END_ACK
        // "is implicit (STREAMING with all seqs confirmed and in-flight table
        // empty)" — this guard enforces that implicit state. A misbehaving caller
        // (sender side) or peer (receiver sending OTA_END_ACK too early) gets
        // PREMATURE instead of a false DONE_OK on an incomplete transfer.
        const bool allSent = (nextSeqToSend_ == totalChunks_);
        const bool allConfirmed = (anyConfirmed_ && highestConfirmedSeq_ + 1 == totalChunks_);
        if (!allSent || !allConfirmed)
        {
            return EndAckResult::premature();
        }

        // OK is the only success status; HASH_MISMATCH and WRITE_ERROR both
        // abandon. The MIXED layer logs the specific status for diagnostics;
        // the state machine only cares about success-vs-abandon. Switch (not
        // if-chain) so a future extension of OtaEndStatus produces a -Wswitch
        // diagnostic at this site rather than silently routing the new
        // enumerator to ABANDONED.
        switch (status)
        {
        case OtaEndStatus::OK:
            status_ = Status::DONE_OK;
            return EndAckResult::doneOk();
        case OtaEndStatus::HASH_MISMATCH:
        case OtaEndStatus::WRITE_ERROR:
            status_ = Status::ABANDONED;
            return EndAckResult::abandoned();
        }
        // Fallback: any future enumerator that lands in OtaEndStatus without
        // an explicit case above is treated as abandon (safe default) but the
        // -Wswitch diagnostic above will have already flagged the gap.
        status_ = Status::ABANDONED;
        return EndAckResult::abandoned();
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
        highWaterSentSeq_ = 0;
        highestConfirmedSeq_ = 0;
        anyConfirmed_ = false;
        inFlight_.fill(InFlightEntry{});
        status_ = Status::IDLE;
    }
} // namespace AstrOsBulkTransport
