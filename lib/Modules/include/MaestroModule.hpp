#ifndef MAESTROMODULE_HPP
#define MAESTROMODULE_HPP

#include <string>
#include <esp_err.h>
#include <hal/uart_types.h>

#define BAUD_RATE_INDICATION 0xAA
#define SET_SERVO_COMMAND 0x84
#define SET_SERVO_SPEED_COMMAND 0x87
#define SET_SERVO_ACCELERATION_COMMAND 0x89
#define GET_SERVO_POSITION_COMMAND 0x90
#define ANY_SERVO_MOVING_STATE_COMMAND 0x93
#define SET_MULTIPLE_SERVOS_COMMAND 0x9F
#define GET_ERROR_COMMAND 0xA1
#define HOME_COMMAND 0xA2

// speed is (.25us/10ms) * n, where n is 0-255
// 0 is no speed limit
// An extended range servo is 500-2500us at 180 degrees
// therefore 11.111...us per degree
// ~(degree/second * .11111) / .25 = speed value
// Speed 1 is ~2.25 degrees/second
// Speed 255 is ~573 degree/second

// acceleration is (0.25us)/(10ms)/(80ms) * n, where n is 0-255
// 0 is no acceleration limit
// An extended range servo is 500-2500us at 180 degrees
// therefore 11.111...us per degree
// ~(degree/second^2 * 11.111) / 312.5 = acceleration value
// Acceleration 1 is ~28 degrees/second^2
// Acceleration 255 is ~7175 degree/second^2

typedef struct
{
    uart_port_t port;
    int baudRate;
    int rxPin;
    int txPin;
} maestro_config_t;

class MaestroModule
{
private:
    bool loading;
    maestro_config_t config;
    esp_err_t InitSerial();
    void setServoPosition(uint8_t channel, int ms, int lastPos, int speed, int acceleration);
    void setServoOff(uint8_t channel);
    int getServoPosition(uint8_t channel);
    void getError();
public:
    MaestroModule();
    ~MaestroModule();
    esp_err_t Init(maestro_config_t config);
    void LoadConfig();
    void HomeServos();
    void QueueCommand(uint8_t *cmd);
    void SetServoPosition(uint8_t channel, int ms);
    void Panic();
    // periodically check servos to turn them off
    void CheckServos(int msSinceLastCheck);
};

extern MaestroModule MaestroMod;

#endif