#ifndef OTAWRITERQUEUEMESSAGE_H
#define OTAWRITERQUEUEMESSAGE_H

#include <stdint.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C"
{
#endif

    // Discriminated union for otaWriterQueue.
    //
    // Memory ownership: producer mallocs every pointer for the kind it sends
    // and on xQueueSend failure calls freeOtaWriterMsg() to release its own
    // allocations. Consumer (otaWriterTask) calls freeOtaWriterMsg() after
    // dispatching to the matching handler. Mixing kinds across the free path
    // will leak or double-free.
    //
    // Per-kind owned pointers:
    //   OTA_WR_BEGIN          none (all inline fixed-size fields)
    //   OTA_WR_DATA           payload (malloc'd by dispatcher because
    //                         parseOtaData returns a pointer INTO the
    //                         packet buffer, which is freed when the
    //                         dispatcher returns)
    //   OTA_WR_END            none (all inline fixed-size fields)
    //   OTA_WR_WATCHDOG_FIRE  none
    //
    // srcMac (BEGIN/DATA/END) is the source MAC of the inbound packet;
    // the consumer uses it as the reply target for ACK/NAK. Inline (not
    // malloc'd) to avoid heap traffic on the chunk hot path.
    //
    // Producers MUST zero-initialize the struct before populating it
    // (e.g., `queue_ota_writer_msg_t m = {};`) so freeOtaWriterMsg's
    // free(m.data.payload) on a non-DATA kind sees a NULL pointer.

    typedef enum
    {
        OTA_WR_BEGIN = 0,
        OTA_WR_DATA = 1,
        OTA_WR_END = 2,
        OTA_WR_WATCHDOG_FIRE = 3
    } ota_writer_msg_kind_t;

    typedef struct
    {
        ota_writer_msg_kind_t kind;

        union
        {
            struct
            {
                uint8_t srcMac[6];
                uint8_t xferId;
                uint32_t totalSize;
                uint16_t chunkSize;
                uint32_t totalChunks;
                uint8_t sha256Expected[32];
                uint8_t flags;
            } begin;

            struct
            {
                uint8_t srcMac[6];
                uint8_t xferId;
                uint32_t seq;
                uint16_t payloadLen;
                uint16_t crc16;
                uint8_t *payload; // malloc'd; freed by freeOtaWriterMsg
            } data;

            struct
            {
                uint8_t srcMac[6];
                uint8_t xferId;
                uint32_t totalChunksSent;
                uint8_t sha256Final[32];
            } end;
            // OTA_WR_WATCHDOG_FIRE has no union arm.
        };
    } queue_ota_writer_msg_t;

    // Sole implementation of the per-kind free contract above. Producer and
    // consumer both call this. Freed pointers are nulled so accidental
    // double-frees become no-ops.
    static inline void freeOtaWriterMsg(queue_ota_writer_msg_t *m)
    {
        if (m == NULL)
        {
            return;
        }
        if (m->kind == OTA_WR_DATA)
        {
            free(m->data.payload);
            m->data.payload = NULL;
        }
        // BEGIN / END / WATCHDOG_FIRE own no heap pointers.
    }

#ifdef __cplusplus
} // extern "C"
#endif

#endif
