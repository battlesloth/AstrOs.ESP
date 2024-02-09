#ifndef SERVOMODULE_HPP
#define SERVOMODULE_HPP

#include <AnimationCommand.hpp>
#include <AstrOsUtility.h>
#include <pca9685.hpp>

#include <esp_system.h>
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
    void QueueCommand(uint8_t *cmd);
    void ZeroServos();
    void MoveServos();
    void Panic();
};

extern ServoModule ServoMod;

#endif