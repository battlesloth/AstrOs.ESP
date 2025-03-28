#include "MaestroCommand.hpp"
#include <esp_log.h>

MaestroCommand::MaestroCommand(std::string val)
{
    str_vec_t parts = SplitTemplate(val);

    if (parts.size() < 7)
    {
        ESP_LOGE("ServoCommand", "Invalid number of parts in command: %s", val.c_str());
        this->channel = -1;
        this->position = -1;
        this->speed = -1;
        this->acceleration = -1;
        return;
    }

    this->controller = parts.at(2);
    this->channel = std::stoi(parts.at(3));
    this->position = std::stoi(parts.at(4));
    this->speed = std::stoi(parts.at(5));
    this->acceleration = std::stoi(parts.at(6));
}

MaestroCommand::~MaestroCommand() {}