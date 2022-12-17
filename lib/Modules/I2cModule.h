#ifndef I2CMODULE_H
#define I2CMODULE_H

#include <AnimationCommand.h>

#include "esp_system.h"
#include <string>

class I2cModule
{
private:
    /* data */
public:
    I2cModule(/* args */);
    ~I2cModule();
    esp_err_t Init();
    void SendCommand(const char* cmd);
};


extern I2cModule I2cMod;

#endif