#include "I2cCommand.hpp"
#include <esp_log.h>

I2cCommand::I2cCommand(std::string val)
{
    str_vec_t parts = SplitTemplate(val);

    if (parts.size() < 4)
    {
        ESP_LOGE("I2cCommand", "Invalid number of parts in command: %s", val.c_str());
        I2cCommand::channel = -1;
        I2cCommand::value = "";
        return;
    }

    I2cCommand::channel = std::stoi(parts.at(2));
    I2cCommand::value = parts.at(3);
}

I2cCommand::~I2cCommand() {}