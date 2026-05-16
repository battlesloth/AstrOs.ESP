#ifndef OTARECEIVER_HPP
#define OTARECEIVER_HPP

#include <AstrOsBulkTransport.hpp>
#include <OtaQueueMessage.h>

#include <cstdint>
#include <string>

// needed for QueueHandle_t, must be in this order
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// Threading: all members of this class are accessed only from otaReceiverTask
// via `process(...)`. No synchronization is required for this class's own
// state. The handler implementations call `AstrOs_SerialMsgHandler.sendFw*(...)`
// to emit FW_*_ACK / NAK / DONE replies; that call terminates in xQueueSend
// against serialQueue and is independently thread-safe.
class OtaReceiver
{
private:
    AstrOsBulkTransport::BulkReceiver bulk_;
    bool active_ = false;
    // Echoed back in every FW_*_ACK / NAK on this transfer.
    std::string transferIdStr_;
    // BEGIN-time msgId; END's own msgId echoes via the END record itself.
    std::string beginMsgId_;

public:
    OtaReceiver();
    ~OtaReceiver();

    // Split from the constructor so the global can be constructed at
    // static-init time, before FreeRTOS queues exist.
    void Init();

    // Frees every malloc'd pointer in the union arm matching msg.kind.
    void process(queue_ota_msg_t &msg);

private:
    void handleBegin(queue_ota_msg_t &msg);
    void handleChunk(queue_ota_msg_t &msg);
    void handleEnd(queue_ota_msg_t &msg);
    void handleDeployBegin(queue_ota_msg_t &msg);
};

extern OtaReceiver AstrOs_OtaReceiver;

#endif
