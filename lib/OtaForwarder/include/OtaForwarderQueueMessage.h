#ifndef OTAFORWARDERQUEUEMESSAGE_H
#define OTAFORWARDERQUEUEMESSAGE_H

#include <stdint.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C"
{
#endif

    // Discriminated union for otaForwarderQueue.
    //
    // Memory ownership: any malloc'd pointer in the active union arm is
    // owned by the producer until xQueueSend succeeds; thereafter the
    // consumer (otaForwarderTask) takes ownership and calls
    // freeOtaForwarderMsg() after dispatch. Most kinds use inline buffers
    // and own nothing beyond transferId — see per-arm notes below.
    //
    // ACK/NAK kinds carry their decoded record fields inline — no pointers,
    // no malloc on the hot path. DEPLOY_BEGIN carries three malloc'd strings
    // (transferId, msgId, orderList). TICK carries nothing.
    //
    // sha256Computed (END_ACK only) is a 32-byte inline buffer; the binary
    // ESP-NOW frame already carries it byte-for-byte and the consumer
    // typically just logs it — no malloc needed.
    //
    // flash_result.reason is a 63-byte inline buffer matching the wire payload's
    // fixed 63 B cap. No malloc needed; producer memcpys reasonLen bytes from
    // the parsed record directly into this buffer.
    //
    // srcMac (ACK/NAK kinds) is a 6-byte inline buffer. Used for forensic
    // logging and to cross-check against the current padawan's MAC.
    //
    // Producers MUST zero-initialize the struct before populating it
    // (e.g., `queue_ota_forwarder_msg_t m = {};` or `memset(&m, 0, sizeof(m))`)
    // because freeOtaForwarderMsg unconditionally free()s transferId — even
    // for kinds that don't own it. Zero-init guarantees that's free(NULL),
    // which is safe; a stack-allocated struct with only `kind` + a union arm
    // set would otherwise read uninitialized memory into free().

    typedef enum
    {
        OTA_FWD_DEPLOY_BEGIN = 0, // from AstrOsSerialMsgHandler (serial side)
        OTA_FWD_BEGIN_ACK = 1,    // from AstrOsEspNow (master role)
        OTA_FWD_BEGIN_NAK = 2,
        OTA_FWD_DATA_ACK = 3,
        OTA_FWD_DATA_NAK = 4,
        OTA_FWD_END_ACK = 5,
        OTA_FWD_TICK = 6,                     // 50 ms tick from esp_timer
        OTA_FWD_STATS_FIRE = 7,               // 2 s periodic stats emission while transfer active
        OTA_FWD_FLASH_RESULT = 8,             // padawan→master flash-commit outcome
        OTA_FWD_FLASH_RESULT_TIMEOUT = 9,     // safety timer fire — padawan never reported
        OTA_FWD_VERSION_CONFIRM_TIMEOUT = 10, // 15 s safety bound — padawan never reported expected version
        OTA_FWD_LOCAL_FLASH_RESULT = 11       // master self-flash: posted by OtaWriter with OK/FAILED + reason
    } ota_forwarder_msg_kind_t;

    typedef struct
    {
        ota_forwarder_msg_kind_t kind;

        // Only populated for OTA_FWD_DEPLOY_BEGIN (malloc'd). Other kinds
        // leave it nullptr. Wire-level opaque string from the originating
        // FW_TRANSFER_BEGIN.
        char *transferId;

        union
        {
            struct
            {
                char *msgId;     // DEPLOY_BEGIN's msgId for FW_DEPLOY_DONE echo
                char *orderList; // RS-separated controller-id list
            } deploy;

            // ACK/NAK arrivals carry the decoded record fields inline.
            // Producer (AstrOsEspNow RX callback) parses the frame via M1's
            // parseOta*Ack/Nak free functions, copies the fields here,
            // posts to the queue.
            struct
            {
                uint8_t srcMac[6];
                uint8_t xferId;
            } begin_ack;

            struct
            {
                uint8_t srcMac[6];
                uint8_t xferId;
                uint8_t reason; // OtaBeginNakReason value
            } begin_nak;

            struct
            {
                uint8_t srcMac[6];
                uint8_t xferId;
                uint32_t highestContiguousSeq;
                uint32_t nextExpectedSeq;
                uint8_t windowRemaining;
            } data_ack;

            struct
            {
                uint8_t srcMac[6];
                uint8_t xferId;
                uint32_t highestContiguousSeq;
                uint32_t nextExpectedSeq;
                uint8_t windowRemaining;
                uint8_t reason; // OtaDataNakReason value
            } data_nak;

            struct
            {
                uint8_t srcMac[6];
                uint8_t xferId;
                uint8_t status; // OtaEndStatus value
                uint8_t sha256Computed[32];
            } end_ack;

            struct
            {
                uint8_t srcMac[6];
                uint8_t xferId;
                uint8_t status;    // OtaFlashStatus value
                uint8_t reasonLen; // 0..63
                char reason[63];   // inline — matches the wire payload's fixed 63 B; no malloc needed
            } flash_result;

            struct
            {
                uint8_t status;         // 0 = OK, non-zero = FAILED
                uint8_t errorReasonLen; // 0..63
                char errorReason[63];   // inline — no malloc needed
            } local_flash_result;
            // OTA_FWD_TICK, OTA_FWD_FLASH_RESULT_TIMEOUT, and
            // OTA_FWD_VERSION_CONFIRM_TIMEOUT have no union arm.
        };
    } queue_ota_forwarder_msg_t;

    // Sole implementation of the per-kind free contract above. Producer and
    // consumer both call this. Freed pointers are nulled so accidental
    // double-frees become no-ops.
    static inline void freeOtaForwarderMsg(queue_ota_forwarder_msg_t *m)
    {
        if (m == NULL)
        {
            return;
        }
        if (m->kind == OTA_FWD_DEPLOY_BEGIN)
        {
            free(m->deploy.msgId);
            free(m->deploy.orderList);
            m->deploy.msgId = NULL;
            m->deploy.orderList = NULL;
        }
        // ACK/NAK, TICK, FLASH_RESULT, FLASH_RESULT_TIMEOUT,
        // VERSION_CONFIRM_TIMEOUT, and LOCAL_FLASH_RESULT kinds have no
        // malloc'd union arm members — nothing to free beyond transferId below.
        // flash_result.reason and local_flash_result.errorReason are inline
        // buffers; no free needed.
        free(m->transferId);
        m->transferId = NULL;
    }

#ifdef __cplusplus
} // extern "C"
#endif

#endif
