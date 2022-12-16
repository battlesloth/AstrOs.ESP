#ifndef PWMMODULE_H
#define PWMMODULE_H

#include <AnimationCommand.h>

#include "esp_system.h"
#include <string>
#include <driver/i2c.h>


class PwmModule
{
private:
public:
    PwmModule();
    ~PwmModule();
    esp_err_t Init(i2c_port_t port, gpio_num_t sda, gpio_num_t scl, uint16_t frequency);
    void SendCommand(const char* cmd);
};


extern PwmModule PwmMod;

#endif