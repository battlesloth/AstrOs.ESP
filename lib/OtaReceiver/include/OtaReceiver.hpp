#ifndef OTARECEIVER_HPP
#define OTARECEIVER_HPP

#include <AstrOsBulkTransport.hpp>
#include <OtaQueueMessage.h>

#include <cstdint>
#include <string>

// needed for QueueHandle_t, must be in this order
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

class OtaReceiver
{
private:
    AstrOsBulkTransport::BulkReceiver bulk_;
    bool active_ = false;
    // Echoed back in every FW_*_ACK / NAK on this transfer.
    std::string transferIdStr_;
    // Captured at BEGIN time, echoed in END_ACK (END's own msgId echoes via
    // the END record itself; this field is for BEGIN_ACK only).
    std::string beginMsgId_;

public:
    OtaReceiver();
    ~OtaReceiver();

    // Called once at boot from main.cpp; intentionally separate from the
    // constructor so the global instance can be constructed at static-init
    // time, before FreeRTOS queues exist.
    void Init();

    // Drains one queue_ota_msg_t. Dispatches by `msg.kind` and frees every
    // malloc'd pointer in the corresponding union arm before returning.
    void process(queue_ota_msg_t &msg);

private:
    void handleBegin(queue_ota_msg_t &msg);
    void handleChunk(queue_ota_msg_t &msg);
    void handleEnd(queue_ota_msg_t &msg);
    void handleDeployBegin(queue_ota_msg_t &msg);
};

extern OtaReceiver AstrOs_OtaReceiver;

#endif
