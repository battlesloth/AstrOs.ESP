#include <GpioModule.hpp>
#include <AnimationCommand.hpp>
#include <AstrOsUtility_ESP.h>

#include <esp_log.h>
#include <string>
#include <vector>
#include <driver/gpio.h>

static const char *TAG = "GpioModule";

GpioModule GpioMod;

GpioModule::GpioModule()
{
}

GpioModule::~GpioModule()
{
}

esp_err_t GpioModule::Init(std::vector<int> channels)
{
    esp_err_t result = ESP_OK;

    for (size_t i = 0; i < channels.size(); i++)
    {
        this->gpioChannels.push_back(channels.at(i));

        if (channels.at(i) != -1)
        {
            result = gpio_set_direction(static_cast<gpio_num_t>(channels.at(i)), GPIO_MODE_OUTPUT);
            if (logError(TAG, "gpio_set_direction", __LINE__, result))
            {
                return result;
            }

            result = gpio_set_level(static_cast<gpio_num_t>(channels.at(i)), 0);
            if (logError(TAG, "gpio_set_level", __LINE__, result))
            {
                return result;
            }
        }
    }

    return result;
}

void GpioModule::SendCommand(uint8_t *cmd)
{
    auto command = GpioCommand(std::string(reinterpret_cast<char *>(cmd)));
    ESP_LOGI(TAG, "Sending Command => %s", cmd);

    if (command.channel > this->gpioChannels.size())
    {
        ESP_LOGE(TAG, "invalid GPIO channel");
        return;
    }

    if (this->gpioChannels.at(command.channel) == -1)
    {
        ESP_LOGE(TAG, "GPIO channel not available");
        return;
    }

    gpio_set_level(static_cast<gpio_num_t>(this->gpioChannels.at(command.channel)), command.state);
}