#include "GpioCommand.hpp"

GpioCommand::GpioCommand(std::string val)
{
    str_vec_t parts = SplitTemplate(val);

    if (parts.size() < 4)
    {
        this->channel = -1;
        this->state = false;
        return;
    }

    this->channel = std::stoi(parts.at(2));
    this->state = std::stoi(parts.at(3));
}

GpioCommand::~GpioCommand() {}
