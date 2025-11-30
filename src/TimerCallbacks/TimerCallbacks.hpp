#ifndef TIMER_CALLBACKS_HPP
#define TIMER_CALLBACKS_HPP

// needed for QueueHandle_t, must be in this order
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <AnimationCommand.hpp>


void pollingCallback(
    bool isMasterNode,
    bool discoveryMode,
    QueueHandle_t espnowQueue,
    bool &polling,
    int &displayTimeout);

void animationCallback(
    CommandTemplate* cmd,
    QueueHandle_t serialCh1Queue,
    QueueHandle_t serialCh2Queue,
    QueueHandle_t servoQueue,
    QueueHandle_t i2cQueue,
    QueueHandle_t gpioQueue);

#endif
