#ifndef GPIOMODULE_HPP
#define GPIOMODULE_HPP

#include <atomic>
#include <esp_system.h>
#include <vector>

class GpioModule
{
private:
    std::vector<int> gpioChannels;
    std::vector<bool> defaults;
    // Last level set per channel, sized once in Init. Written by SendCommand
    // (gpio queue task) and DefaultGpios (boot / RELOAD_CONFIG), read by
    // GetLevel (random-effects task, T-009).
    std::vector<std::atomic<bool>> levels;

public:
    GpioModule();
    ~GpioModule();
    esp_err_t Init(std::vector<int> channels);
    void UpdateConfig(std::vector<bool> config);
    void DefaultGpios();
    void SendCommand(uint8_t *cmd);
    // The level this module last set on channel (low until set). False when
    // channel is out of range.
    bool GetLevel(int channel, bool *out) const;
};

extern GpioModule GpioMod;

#endif