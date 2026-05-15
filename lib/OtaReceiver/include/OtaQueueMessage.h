#ifndef OTAQUEUEMESSAGE_H
#define OTAQUEUEMESSAGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    typedef enum
    {
        OTA_MSG_BEGIN = 0,
        OTA_MSG_CHUNK = 1,
        OTA_MSG_END = 2,
        OTA_MSG_DEPLOY_BEGIN = 3
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
    //   OTA_MSG_BEGIN         transferId, msgId, targetList
    //   OTA_MSG_CHUNK         transferId, payload
    //   OTA_MSG_END           transferId, msgId
    //   OTA_MSG_DEPLOY_BEGIN  transferId, msgId, orderList
    //
    // sha256Hex / finalSha256Hex are fixed-size 65-byte buffers (64 hex
    // chars + NUL); they live inline in the struct and never need freeing.
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
                char sha256Hex[65];
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
                char finalSha256Hex[65];
            } end;

            struct
            {
                char *msgId;     // DEPLOY_BEGIN's msgId for DONE echo
                char *orderList; // RS-separated controller-id list
            } deploy;
        };
    } queue_ota_msg_t;

#ifdef __cplusplus
} // extern "C"
#endif

#endif
