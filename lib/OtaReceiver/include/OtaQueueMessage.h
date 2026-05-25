#ifndef OTAQUEUEMESSAGE_H
#define OTAQUEUEMESSAGE_H

#include <stdint.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C"
{
#endif

    typedef enum
    {
        OTA_MSG_BEGIN = 0,
        OTA_MSG_CHUNK = 1,
        OTA_MSG_END = 2,
        // Posted by the idle-activity watchdog when no chunk activity is seen
        // within the threshold. No payload; transferId is nullptr.
        OTA_MSG_WATCHDOG_FIRE = 3
    } ota_msg_kind_t;

    // Discriminated union carrying one decoded inbound FW_* message from
    // AstrOsSerialMsgHandler (producer) to otaReceiverTask (consumer).
    //
    // Memory ownership: producer mallocs every pointer for the kind it sends
    // and on xQueueSend failure calls freeOtaMsg() to release its own
    // allocations. Consumer calls freeOtaMsg() after dispatching to the
    // matching handler. Mixing kinds across the free path will leak or
    // double-free.
    //
    // Per-kind owned pointers:
    //   OTA_MSG_BEGIN          transferId, msgId, targetList
    //   OTA_MSG_CHUNK          transferId, payload
    //   OTA_MSG_END            transferId, msgId
    //   OTA_MSG_WATCHDOG_FIRE  none (transferId is nullptr; no union arm)
    //
    // sha256Hex / finalSha256Hex are inline 65-byte buffers (64 hex chars +
    // NUL). Inline so the chunk hot path doesn't pay a malloc/free per BEGIN.
    // Scoped as enum (not #define) so the constant doesn't leak into every
    // including translation unit.
    enum
    {
        SHA256_HEX_LEN = 64
    };

    typedef struct
    {
        ota_msg_kind_t kind;
        char *transferId; // wire-level opaque string, malloc'd, NUL-terminated

        union
        {
            struct
            {
                char *msgId; // BEGIN's msgId for ACK echo
                uint32_t totalSize;
                uint32_t totalChunks;
                uint16_t chunkSize;
                char sha256Hex[SHA256_HEX_LEN + 1];
                char *targetList; // RS-separated controller-id list
            } begin;

            struct
            {
                uint32_t seq;
                uint16_t payloadLen; // base64-DECODED length, bytes
                uint16_t crc16;
                uint8_t *payload; // decoded bytes, length == payloadLen
            } chunk;

            struct
            {
                char *msgId; // END's msgId for ACK echo
                uint32_t totalChunks;
                char finalSha256Hex[SHA256_HEX_LEN + 1];
            } end;
        };
    } queue_ota_msg_t;

    // Sole implementation of the per-kind free contract above. Both producer
    // and consumer call this. Freed pointers are nulled so accidental
    // double-frees become no-ops.
    static inline void freeOtaMsg(queue_ota_msg_t *m)
    {
        if (m == NULL)
        {
            return;
        }
        switch (m->kind)
        {
        case OTA_MSG_BEGIN:
            free(m->begin.msgId);
            free(m->begin.targetList);
            m->begin.msgId = NULL;
            m->begin.targetList = NULL;
            break;
        case OTA_MSG_CHUNK:
            free(m->chunk.payload);
            m->chunk.payload = NULL;
            break;
        case OTA_MSG_END:
            free(m->end.msgId);
            m->end.msgId = NULL;
            break;
        case OTA_MSG_WATCHDOG_FIRE:
            // No union arm; transferId is nullptr by contract.
            break;
        default:
            // Unknown kind — caller must log. Only transferId is freed below;
            // the union arm leaks because its layout can't be inferred.
            break;
        }
        free(m->transferId);
        m->transferId = NULL;
    }

#ifdef __cplusplus
} // extern "C"
#endif

#endif
