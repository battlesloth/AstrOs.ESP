#ifndef SERVOMODULE_H
#define SERVOMODULE_H

#include <AnimationCommand.h>
#include "AstrOsUtility.h"
#include "Pca9685.h"

#include "esp_system.h"
#include <string>
#include <driver/i2c.h>




class ServoModule
{
private:
    void SetCommandByBoard(servo_channel *servos, ServoCommand *cmd);
    void MoveServoByBoard(Pca9685 *board, servo_channel *servos, int idx);
public:
    ServoModule();
    ~ServoModule();
    esp_err_t Init(uint8_t board0addr, uint8_t board1addr);
    void LoadServoConfig();
    void QueueCommand(const char* cmd);
    void ZeroServos();
    void MoveServos();
    void Panic();
};


extern ServoModule ServoMod;

#endif