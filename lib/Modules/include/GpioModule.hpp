#ifndef GPIOMODULE_HPP
#define GPIOMODULE_HPP

#include <esp_system.h>
#include <vector>

class GpioModule
{
private:
    std::vector<int> gpioChannels;

public:
    GpioModule();
    ~GpioModule();
    esp_err_t Init(std::vector<int> channels);
    void SendCommand(uint8_t *cmd);
};

extern GpioModule GpioMod;

#endif