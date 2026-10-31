#include <AnimationCommands.hpp>
#include <AstrOsUtility_ESP.h>
#include <GpioModule.hpp>

#include <driver/gpio.h>
#include <esp_log.h>
#include <string>
#include <vector>

static const char *TAG = "GpioModule";
static const int LEVEL_MUTEX_WAIT_MS = 100;

GpioModule GpioMod;

GpioModule::GpioModule() {}

GpioModule::~GpioModule() {}

esp_err_t GpioModule::Init(std::vector<int> channels)
{
    esp_err_t result = ESP_OK;

    this->levelMutex = xSemaphoreCreateMutex();
    if (this->levelMutex == NULL)
    {
        ESP_LOGE(TAG, "Failed to create the GPIO level mutex");
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < channels.size(); i++)
    {
        this->gpioChannels.push_back(channels.at(i));

        if (channels.at(i) != 0)
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

    this->levels = std::vector<std::atomic<bool>>(this->gpioChannels.size());

    return result;
}

void GpioModule::UpdateConfig(std::vector<bool> config)
{

    this->defaults.clear();

    for (size_t i = 0; i < config.size(); i++)
    {
        this->defaults.push_back(config.at(i));
    }

    this->DefaultGpios();
}

void GpioModule::DefaultGpios()
{
    if (xSemaphoreTake(this->levelMutex, pdMS_TO_TICKS(LEVEL_MUTEX_WAIT_MS)) != pdTRUE)
    {
        ESP_LOGE(TAG, "DefaultGpios: level mutex timeout, defaults not applied");
        return;
    }

    for (size_t i = 0; i < this->gpioChannels.size(); i++)
    {
        if (this->defaults.at(i) != -1 && this->gpioChannels.at(i) != -1)
        {
            gpio_set_level(static_cast<gpio_num_t>(this->gpioChannels.at(i)), this->defaults.at(i));
            this->levels.at(i).store(this->defaults.at(i));
        }
        else if (this->gpioChannels.at(i) != -1)
        {
            gpio_set_level(static_cast<gpio_num_t>(this->gpioChannels.at(i)), false);
            this->levels.at(i).store(false);
        }
    }

    xSemaphoreGive(this->levelMutex);
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

    if (xSemaphoreTake(this->levelMutex, pdMS_TO_TICKS(LEVEL_MUTEX_WAIT_MS)) != pdTRUE)
    {
        ESP_LOGE(TAG, "SendCommand: level mutex timeout, GPIO channel %d not set", command.channel);
        return;
    }
    gpio_set_level(static_cast<gpio_num_t>(this->gpioChannels.at(command.channel)), command.state);
    this->levels.at(command.channel).store(command.state);
    xSemaphoreGive(this->levelMutex);
}

bool GpioModule::GetLevel(int channel, bool *out) const
{
    if (channel < 0 || static_cast<size_t>(channel) >= this->levels.size())
    {
        return false;
    }

    *out = this->levels[channel].load();
    return true;
}