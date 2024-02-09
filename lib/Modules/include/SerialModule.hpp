#ifndef SERIALMODULE_HPP
#define SERIALMODULE_HPP

#include <AnimationCommand.hpp>

#include <esp_system.h>
#include <string>
#include <driver/uart.h>

typedef struct
{
    int baudRate1;
    int rxPin1;
    int txPin1;
    int baudRate2;
    int rxPin2;
    int txPin2;
    int baudRate3;
    int rxPin3;
    int txPin3;
} serial_config_t;

class SerialModule
{
private:
    void SendData(int ch, const uint8_t *data, size_t size);
    esp_err_t InstallSerial(uart_port_t port, int tx, int rx, int baud);
    void SoftSerialWrite(const char *msg);
    int currentSerial;
    int tx[3];
    int rx[3];
    int baud[3];

public:
    SerialModule(/* args */);
    ~SerialModule();
    esp_err_t Init(serial_config_t cfig);
    void SendCommand(uint8_t *cmd);
    void SendBytes(int ch, uint8_t *data, size_t size);
};

extern SerialModule SerialMod;

#endif