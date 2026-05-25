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
    // Build a synthetic OTA_WR_DATA message with a malloc'd payload, then
    // assert freeOtaWriterMsg releases the malloc + nulls the pointer.
    queue_ota_writer_msg_t makeDataMsg(uint8_t xferId, uint32_t seq, const uint8_t *bytes, uint16_t len)
    {
        queue_ota_writer_msg_t m{};
        m.kind = OTA_WR_DATA;
        memset(m.data.srcMac, 0, 6);
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
    memset(m.begin.srcMac, 0, 6);
    m.begin.xferId = 7;
    m.begin.totalSize = 1024;
    m.begin.totalChunks = 8;
    m.begin.chunkSize = 128;
    memset(m.begin.sha256Expected, 0xAB, 32);
    m.begin.flags = 0;

    // No pointers owned by BEGIN; freeOtaWriterMsg is a no-op for it.
    // The assertion is "doesn't crash" — a real leak would surface as a
    // valgrind/ASAN finding under host CI in the future.
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
    memset(m.end.srcMac, 0, 6);
    m.end.xferId = 3;
    m.end.totalChunksSent = 8;
    memset(m.end.sha256Final, 0x12, 32);

    freeOtaWriterMsg(&m);
    EXPECT_EQ(3, m.end.xferId);
}

TEST(OtaWriterQueueMsg, FreeWatchdogIsSafe)
{
    queue_ota_writer_msg_t m{};
    m.kind = OTA_WR_WATCHDOG_FIRE;
    // No payload; freeOtaWriterMsg must tolerate the zero-init.
    freeOtaWriterMsg(&m);
}

TEST(OtaWriterQueueMsg, DoubleFreeIsSafe)
{
    uint8_t scratch[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    queue_ota_writer_msg_t m = makeDataMsg(/*xferId=*/1, /*seq=*/0, scratch, sizeof(scratch));
    freeOtaWriterMsg(&m); // first free
    freeOtaWriterMsg(&m); // second free must be no-op (pointer nulled)
}
