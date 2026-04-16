#include "MaestroCommand.hpp"

MaestroCommand::MaestroCommand(std::string val)
{
    str_vec_t parts = SplitTemplate(val);

    if (parts.size() < 7)
    {
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
