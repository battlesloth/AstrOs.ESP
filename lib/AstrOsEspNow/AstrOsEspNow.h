#ifndef ASTROSESPNOW_H
#define ASTROSESPNOW_H

#include "AstrOsEspNowUtility.h"
#include <esp_err.h>

class AstrOsEspNow
{
private:
public:
    AstrOsEspNow();
    ~AstrOsEspNow();
    esp_err_t init();
    esp_err_t addPeer(uint8_t *macAddress);
    queue_espnow_msg_t generateRegisterMessage(uint8_t *macAddress);
};

extern AstrOsEspNow AstrOs_EspNow;

#endif