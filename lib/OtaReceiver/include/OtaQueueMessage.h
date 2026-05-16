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
        OTA_MSG_DEPLOY_BEGIN = 3,
        // Posted by OtaReceiver's idle-activity watchdog (esp_timer one-shot)
        // back to the otaQueue when no chunk activity is seen within the
        // configured idle threshold. Carries no payload — `transferId` is
        // nullptr and no union arm is used. Handler clears in-progress
        // state so the next FW_TRANSFER_BEGIN can succeed without reboot.
        OTA_MSG_WATCHDOG_FIRE = 4
    } ota_msg_kind_t;

    // Discriminated union carrying one decoded inbound FW_* message from
    // AstrOsSerialMsgHandler (producer) to otaReceiverTask (consumer).
    //
    // Memory ownership:
    //   - Producer mallocs every pointer field listed below for the kind
    //     it sends. On xQueueSend failure, producer frees its own
    //     allocations and emits a fallback NAK so the server can retry.
    //   - Consumer reads `kind` first, then frees only the pointers
    //     belonging to that kind's union arm. Mixing kinds across the
    //     free path will leak or double-free.
    //
    // Per-kind owned pointers:
    //   OTA_MSG_BEGIN          transferId, msgId, targetList
    //   OTA_MSG_CHUNK          transferId, payload
    //   OTA_MSG_END            transferId, msgId
    //   OTA_MSG_DEPLOY_BEGIN   transferId, msgId, orderList
    //   OTA_MSG_WATCHDOG_FIRE  none (transferId is nullptr; no union arm)
    //
    // sha256Hex / finalSha256Hex are fixed-size buffers sized for the 64
    // hex chars + NUL terminator; they live inline in the struct and never
    // need freeing. SHA256_HEX_LEN names the hex-string length so the
    // strncpy(buf, src, SHA256_HEX_LEN) + buf[SHA256_HEX_LEN]='\0' idiom
    // reads as one constant instead of a 64/65 magic-number pair.
    // Defined as an enum (not #define) to keep it scoped — the macro form
    // would leak into every translation unit that includes this header.
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

            struct
            {
                char *msgId;     // DEPLOY_BEGIN's msgId for DONE echo
                char *orderList; // RS-separated controller-id list
            } deploy;
        };
    } queue_ota_msg_t;

    // Centralizes the per-kind free contract documented above. Both the
    // producer's xQueueSend-fail branches and the consumer's process()
    // success branches call this so the per-kind ownership rule lives in
    // exactly one place. All freed pointers are cleared to nullptr to make
    // accidental double-frees no-ops.
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
        case OTA_MSG_DEPLOY_BEGIN:
            free(m->deploy.msgId);
            free(m->deploy.orderList);
            m->deploy.msgId = NULL;
            m->deploy.orderList = NULL;
            break;
        case OTA_MSG_WATCHDOG_FIRE:
            // No union arm; transferId is nullptr by contract.
            break;
        default:
            // Unknown kind — caller (e.g. OtaReceiver::process) has already
            // logged the missed enumerator. We can't know which union arm is
            // live, so only the transferId is freed below. The union arm
            // leaks; this is the price of an out-of-range kind, and it's
            // unreachable in normal operation (the kind is set by the
            // producer immediately before send).
            break;
        }
        free(m->transferId);
        m->transferId = NULL;
    }

#ifdef __cplusplus
} // extern "C"
#endif

#endif
