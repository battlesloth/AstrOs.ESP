#include <I2cModule.h>
#include <AnimationCommand.h>
#include <AstrOsDisplay.hpp>
#include <AstrOsUtility.h>

#include "esp_system.h"
#include <esp_log.h>
#include <driver/i2c.h>
#include <sstream>
#include <string>
#include "ssd1306.h"

static const char *TAG = "I2cModule";
static pthread_mutex_t i2cMutex;

SSD1306_t oled;

I2cModule I2cMod;
;

I2cModule::I2cModule(/* args */)
{
}

I2cModule::~I2cModule()
{
}

esp_err_t I2cModule::Init()
{
    esp_err_t result = ESP_OK;

    if (pthread_mutex_init(&i2cMutex, NULL) != 0)
    {
        ESP_LOGE(TAG, "Failed to initialize the I2C mutex");
    }

#ifdef USE_I2C_OLED
    oled._address = I2CAddress;
    oled._flip = false;
    ssd1306_init(&oled, 128, 32);
    ssd1306_clear_screen(&oled, false);
    ssd1306_contrast(&oled, 0xff);
    I2cModule::writeSsd1306(0, "AstrOs");
    I2cModule::writeSsd1306(1, AstrOsConstants::Version);
    I2cModule::writeSsd1306(2, "Starting...");
#endif

    return result;
}

void I2cModule::SendCommand(uint8_t *cmd)
{
    auto command = I2cCommand(std::string(reinterpret_cast<char *>(cmd)));

    ESP_LOGI(TAG, "Sending Command => %s", cmd);

    I2cModule::write(command.channel, (uint8_t *)command.value.c_str(), command.value.size() + 1);
}

esp_err_t I2cModule::write(uint8_t addr, uint8_t *data, size_t size)
{
    esp_err_t ret = ESP_OK;
    if (pthread_mutex_lock(&i2cMutex) == 0)
    {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | WRITE_BIT, ACK_CHECK_EN);
        i2c_master_write(cmd, data, size, ACK_CHECK_EN);
        i2c_master_stop(cmd);
        ret = i2c_master_cmd_begin(I2C_NUM_0, cmd, 1000 / portTICK_PERIOD_MS);
        i2c_cmd_link_delete(cmd);
        pthread_mutex_unlock(&i2cMutex);
    }
    return ret;
}

void I2cModule::WriteDisplay(uint8_t *cmd)
{
#ifdef USE_I2C_OLED
    auto command = DisplayCommand(std::string(reinterpret_cast<char *>(cmd)));

    if (command.useLine1)
    {
        I2cModule::writeSsd1306(0, command.line1);
    }
    else
    {
        I2cModule::clearSsd1306(0);
    }
    if (command.useLine2)
    {
        I2cModule::writeSsd1306(1, command.line2);
    }
    else
    {
        I2cModule::clearSsd1306(1);
    }
    if (command.useLine3)
    {
        I2cModule::writeSsd1306(2, command.line3);
    }
    else
    {
        I2cModule::clearSsd1306(2);
    }
#endif
}

void I2cModule::writeSsd1306(int line, std::string value)
{
    int len = 0;
    std::stringstream ss;

    if (value.length() >= 16)
    {
        len = 16;
        ss << value;
    }
    else
    {
        int remainder = 16 - value.length();
        int spaces = (remainder + 2 - 1) / 2;
        ss << std::string(spaces, ' ') << value;
        len = value.length() + spaces;
    }

    char *c = const_cast<char *>(ss.str().c_str());

    if (pthread_mutex_lock(&i2cMutex) == 0)
    {
        ssd1306_clear_line(&oled, line, false);
        ssd1306_display_text(&oled, line, c, len, false);
        pthread_mutex_unlock(&i2cMutex);
    }
}

void I2cModule::clearSsd1306(int line)
{
    if (pthread_mutex_lock(&i2cMutex) == 0)
    {
        ssd1306_clear_line(&oled, line, false);
        pthread_mutex_unlock(&i2cMutex);
    }
}