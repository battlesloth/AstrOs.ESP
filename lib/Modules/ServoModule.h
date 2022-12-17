#ifndef SERVOMODULE_H
#define SERVOMODULE_H

#include <AnimationCommand.h>

#include "esp_system.h"
#include <string>
#include <driver/i2c.h>




class ServoModule
{
private:

public:
    ServoModule();
    ~ServoModule();
    esp_err_t Init(i2c_port_t port, gpio_num_t sda, gpio_num_t scl, uint16_t frequency);
    void LoadServoConfig();
    void QueueCommand(const char* cmd);
    void ZeroServos();
    void MoveServos();
    void Panic();
};


extern ServoModule ServoMod;

#endif