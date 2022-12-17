#ifndef SERIALMODULE_H
#define SERIALMODULE_H

#include <AnimationCommand.h>

#include "esp_system.h"
#include <string>

class SerialModule
{
private:
    void SendData(const char* cmd);
public:
    SerialModule(/* args */);
    ~SerialModule();
    esp_err_t Init(int baud_rate, int rx_pin, int tx_pin);
    void SendCommand(const char* cmd);
};


extern SerialModule SerialMod;


#endif