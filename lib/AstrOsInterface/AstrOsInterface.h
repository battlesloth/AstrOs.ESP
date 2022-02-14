#ifndef ASTROSINTERFACE_H
#define ASTROSINTERFACE_H

// needed for QueueHandle_t, must be in this order
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>


class AstrOsInterface{
    private:
    QueueHandle_t animationQueue;
    public:
    AstrOsInterface();
    ~AstrOsInterface();
    void Init(QueueHandle_t animationQueue);
    void handleMessage(char *msg);
};

extern AstrOsInterface AstrOs;

#endif