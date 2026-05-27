#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <OtaWriterQueueMessage.h>

#include <cstdlib>
#include <cstring>

// These tests pin the per-kind ownership contract of queue_ota_writer_msg_t.
// The actual OtaWriter class is MIXED and cannot link in [env:test]; the
// queue-message header is plain C and links cleanly. Pointer-ownership bugs
// here would manifest as random bytes appearing in the flashed partition
// (OTA_WR_DATA) or as leaks (OTA_WR_BEGIN's sha bytes — though Begin owns
// none, the test pins that contract too).

namespace
{
    queue_ota_writer_msg_t makeDataMsg(uint8_t xferId, uint32_t seq, const uint8_t *bytes, uint16_t len)
    {
        queue_ota_writer_msg_t m{};
        m.kind = OTA_WR_DATA;
        memset(m.data.srcMac, 0, sizeof(m.data.srcMac));
        m.data.xferId = xferId;
        m.data.seq = seq;
        m.data.payloadLen = len;
        m.data.crc16 = 0xBEEF;
        m.data.payload = static_cast<uint8_t *>(malloc(len));
        memcpy(m.data.payload, bytes, len);
        return m;
    }
} // namespace

TEST(OtaWriterQueueMsg, FreeNullIsNoop)
{
    freeOtaWriterMsg(nullptr); // must not crash
}

TEST(OtaWriterQueueMsg, FreeBeginReleasesNothing)
{
    queue_ota_writer_msg_t m{};
    m.kind = OTA_WR_BEGIN;
    memset(m.begin.srcMac, 0, sizeof(m.begin.srcMac));
    m.begin.xferId = 7;
    m.begin.totalSize = 1024;
    m.begin.totalChunks = 8;
    m.begin.chunkSize = 128;
    memset(m.begin.sha256Expected, 0xAB, sizeof(m.begin.sha256Expected));
    m.begin.flags = 0;

    // BEGIN owns no pointers; the assertion is just "doesn't crash".
    freeOtaWriterMsg(&m);
    EXPECT_EQ(7, m.begin.xferId); // POD fields untouched
}

TEST(OtaWriterQueueMsg, FreeDataReleasesPayload)
{
    uint8_t scratch[16];
    memset(scratch, 0xCD, sizeof(scratch));

    queue_ota_writer_msg_t m = makeDataMsg(/*xferId=*/9, /*seq=*/42, scratch, sizeof(scratch));
    ASSERT_NE(nullptr, m.data.payload);

    freeOtaWriterMsg(&m);
    EXPECT_EQ(nullptr, m.data.payload); // nulled after free
}

TEST(OtaWriterQueueMsg, FreeEndReleasesNothing)
{
    queue_ota_writer_msg_t m{};
    m.kind = OTA_WR_END;
    memset(m.end.srcMac, 0, sizeof(m.end.srcMac));
    m.end.xferId = 3;
    m.end.totalChunksSent = 8;
    memset(m.end.sha256Final, 0x12, sizeof(m.end.sha256Final));

    freeOtaWriterMsg(&m);
    EXPECT_EQ(3, m.end.xferId);
}

TEST(OtaWriterQueueMsg, FreeWatchdogIsSafe)
{
    queue_ota_writer_msg_t m{};
    m.kind = OTA_WR_WATCHDOG_FIRE;
    freeOtaWriterMsg(&m);
}

TEST(OtaWriterQueueMsg, DoubleFreeIsSafe)
{
    uint8_t scratch[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    queue_ota_writer_msg_t m = makeDataMsg(/*xferId=*/1, /*seq=*/0, scratch, sizeof(scratch));
    freeOtaWriterMsg(&m); // first free
    freeOtaWriterMsg(&m); // second free must be no-op (pointer nulled)
}
