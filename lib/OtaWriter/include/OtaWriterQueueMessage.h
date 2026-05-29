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
    //   OTA_WR_BEGIN              none (all inline fixed-size fields)
    //   OTA_WR_DATA               payload (malloc'd by dispatcher because
    //                             parseOtaData returns a pointer INTO the
    //                             packet buffer, which is freed when the
    //                             dispatcher returns)
    //   OTA_WR_END                none (all inline fixed-size fields)
    //   OTA_WR_WATCHDOG_FIRE      none
    //   OTA_WR_LOCAL_FLASH_REQ    none (all inline fixed-size fields)
    //
    // srcMac (BEGIN/DATA/END) is the source MAC of the inbound packet;
    // the consumer uses it as the reply target for ACK/NAK. Inline (not
    // malloc'd) to avoid heap traffic on the chunk hot path.
    //
    // freeOtaWriterMsg only frees data.payload when kind == OTA_WR_DATA, so
    // zero-init isn't required for the current free path. It's still
    // recommended hygiene (`queue_ota_writer_msg_t m = {};`) — if the free
    // helper is ever made kind-agnostic, the inactive union arms must already
    // be NULL.

    typedef enum
    {
        OTA_WR_BEGIN = 0,
        OTA_WR_DATA = 1,
        OTA_WR_END = 2,
        OTA_WR_WATCHDOG_FIRE = 3,
        OTA_WR_STATS_FIRE = 4,     // 2 s periodic stats emission while transfer active
        OTA_WR_LOCAL_FLASH_REQ = 5 // master self-flash: posted by OtaForwarder with firmware path + expected size + SHA
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

            struct
            {
                char firmwarePath[64];
                uint32_t expectedSize;
                uint8_t expectedSha256[32];
            } local_flash_req;
            // OTA_WR_WATCHDOG_FIRE and OTA_WR_STATS_FIRE have no union arm.
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
        // BEGIN / END / WATCHDOG_FIRE / STATS_FIRE / LOCAL_FLASH_REQ own no heap pointers.
    }

#ifdef __cplusplus
} // extern "C"
#endif

#endif
