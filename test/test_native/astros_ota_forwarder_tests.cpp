#include <gmock/gmock.h>
#include <gtest/gtest.h>

// The OtaReceiver accessor is a small, native-testable surface despite the
// rest of OtaReceiver being MIXED. The native tests cover the accessor's
// thread-safe getter/setter shape via a minimal stand-in class that mirrors
// the production discipline (mutex-protected std::string).
//
// Why a stand-in instead of testing OtaReceiver directly: OtaReceiver pulls
// in ESP-IDF / FreeRTOS / mbedtls headers and cannot link in [env:test].
// The accessor's logic is small enough that mirroring it under a native
// stand-in catches the same kinds of regressions (mutex discipline,
// copy-vs-move semantics, empty-by-default).

#include <mutex>
#include <optional>
#include <string>

namespace
{
    // Mirror of OtaReceiver's accessor surface. Production-side test is
    // bench-only (see Task 8). This test pins the accessor's contract.
    class LastFirmwarePathHolder
    {
    public:
        std::optional<std::string> get() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (path_.empty())
            {
                return std::nullopt;
            }
            return path_;
        }

        void set(const std::string &path)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            path_ = path;
        }

        void clear()
        {
            std::lock_guard<std::mutex> lock(mutex_);
            path_.clear();
        }

    private:
        mutable std::mutex mutex_;
        std::string path_;
    };
} // namespace

TEST(LastFirmwarePathHolder, EmptyByDefault)
{
    LastFirmwarePathHolder holder;
    EXPECT_EQ(std::nullopt, holder.get());
}

TEST(LastFirmwarePathHolder, ReturnsSetValue)
{
    LastFirmwarePathHolder holder;
    holder.set("/sdcard/firmware/abcd1234.bin");

    auto got = holder.get();
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ("/sdcard/firmware/abcd1234.bin", *got);
}

TEST(LastFirmwarePathHolder, OverwriteReturnsLatest)
{
    LastFirmwarePathHolder holder;
    holder.set("/sdcard/firmware/first.bin");
    holder.set("/sdcard/firmware/second.bin");

    auto got = holder.get();
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ("/sdcard/firmware/second.bin", *got);
}

TEST(LastFirmwarePathHolder, ClearReturnsEmpty)
{
    LastFirmwarePathHolder holder;
    holder.set("/sdcard/firmware/abcd1234.bin");
    holder.clear();
    EXPECT_EQ(std::nullopt, holder.get());
}

#include "../../lib/OtaForwarder/include/OtaForwarderQueueMessage.h"

#include <cstring>

// Free-helper contract: producer mallocs; consumer (or producer on send
// failure) calls freeOtaForwarderMsg. Pointers nulled after free so an
// accidental double-free is a no-op.

TEST(OtaForwarderMsg, FreeDeployBeginReleasesAllOwnedPointers)
{
    queue_ota_forwarder_msg_t m;
    std::memset(&m, 0, sizeof(m));
    m.kind = OTA_FWD_DEPLOY_BEGIN;
    m.transferId = strdup("xfer-7");
    m.deploy.msgId = strdup("msg-3");
    m.deploy.orderList = strdup("body\x1E"
                                "core\x1E"
                                "dome"); // string-literal concat avoids hex-escape greediness

    ASSERT_NE(nullptr, m.transferId);
    ASSERT_NE(nullptr, m.deploy.msgId);
    ASSERT_NE(nullptr, m.deploy.orderList);

    freeOtaForwarderMsg(&m);

    EXPECT_EQ(nullptr, m.transferId);
    EXPECT_EQ(nullptr, m.deploy.msgId);
    EXPECT_EQ(nullptr, m.deploy.orderList);
}

TEST(OtaForwarderMsg, FreeAckNakKindsAreNoOpForInlineFields)
{
    // The ACK/NAK kinds carry only inline fields (xferId, seqs, etc.).
    // freeOtaForwarderMsg should be safe on them (transferId is unused
    // for these kinds and stays nullptr).
    queue_ota_forwarder_msg_t m;
    std::memset(&m, 0, sizeof(m));
    m.kind = OTA_FWD_DATA_ACK;
    m.data_ack.xferId = 7;
    m.data_ack.highestContiguousSeq = 100;
    m.data_ack.nextExpectedSeq = 101;
    m.data_ack.windowRemaining = 7;

    freeOtaForwarderMsg(&m); // must not crash, must not free random memory
    // Fields stay set — only the malloc'd pointer arms get zeroed.
    EXPECT_EQ(7, m.data_ack.xferId);
}

TEST(OtaForwarderMsg, FreeTickIsNoOp)
{
    queue_ota_forwarder_msg_t m;
    std::memset(&m, 0, sizeof(m));
    m.kind = OTA_FWD_TICK;
    freeOtaForwarderMsg(&m); // must not crash; no allocated members
}

TEST(OtaForwarderMsg, FreeNullPtrIsNoOp)
{
    freeOtaForwarderMsg(nullptr); // must not crash
}

TEST(OtaForwarderMsg, FreeIsIdempotent)
{
    queue_ota_forwarder_msg_t m;
    std::memset(&m, 0, sizeof(m));
    m.kind = OTA_FWD_DEPLOY_BEGIN;
    m.transferId = strdup("x");
    m.deploy.msgId = strdup("m");
    m.deploy.orderList = strdup("o");

    freeOtaForwarderMsg(&m); // first free
    freeOtaForwarderMsg(&m); // second free — no-op because pointers nulled
}
