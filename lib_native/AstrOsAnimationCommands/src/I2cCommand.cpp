#include "I2cCommand.hpp"

I2cCommand::I2cCommand(std::string val)
{
    str_vec_t parts = SplitTemplate(val);

    if (parts.size() < 4)
    {
        this->channel = -1;
        this->value = "";
        return;
    }

    this->channel = std::stoi(parts.at(2));
    this->value = parts.at(3);
}

I2cCommand::~I2cCommand() {}
