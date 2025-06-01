#ifndef SERIALMODULE_HPP
#define SERIALMODULE_HPP

#include <AnimationCommand.hpp>

#include <esp_system.h>
#include <string>
#include <driver/uart.h>

typedef struct
{
    uart_port_t port;
    int defaultBaudRate;
    int rxPin;
    int txPin;
    bool isMaster;
} serial_config_t;

class SerialModule
{
private:
    void SendData(int baud, const uint8_t *data, size_t size);
    esp_err_t InstallSerial(uart_port_t port, int tx, int rx, int baud);
   
    uart_port_t port;
    int tx;
    int rx;
    int defaultBaudrate;
    bool isMaster;
public:
    SerialModule(/* args */);
    ~SerialModule();
    esp_err_t Init(serial_config_t cfig);
    void SendCommand(uint8_t *cmd);
    void SendBytes(int baud, uint8_t *data, size_t size);
};

#endif