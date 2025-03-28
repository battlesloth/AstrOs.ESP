#include <SerialCommand.hpp>
#include <AstrOsStringUtils.hpp>
#include <esp_log.h>

SerialCommand::SerialCommand(std::string val)
{

    str_vec_t parts = SplitTemplate(val);

    if (parts.size() < 4)
    {
        ESP_LOGE("SerialCommand", "Invalid number of parts in command: %s", val.c_str());
        this->type = AnimationCmdType::NONE;
        this->serialChannel = -1;
        this->baudRate = -1;
        this->ch = -1;
        this->cmd = -1;
        this->spd = -1;
        this->pos = -1;
        this->value = "";
        return;
    }

    this->type = static_cast<AnimationCmdType>(std::stoi(parts.at(0)));

    this->serialChannel = std::stoi(parts.at(2));
    this->baudRate = std::stoi(parts.at(3));

    if (this->type == AnimationCmdType::KANGAROO)
    {
        this->ch = std::stoi(parts.at(4));
        this->cmd = std::stoi(parts.at(5));
        this->spd = std::stoi(parts.at(6));
        this->pos = std::stoi(parts.at(7));
    }
    else
    {
        this->value = parts.at(4);
    }
}

SerialCommand::SerialCommand() {}

SerialCommand::~SerialCommand() {}

std::string SerialCommand::GetValue()
{
    if (this->type == AnimationCmdType::KANGAROO)
    {
        return ToKangarooCommand();
    }
    else
    {
        return this->value;
    }
}

std::string SerialCommand::ToKangarooCommand()
{
    switch (this->cmd)
    {
    case KangarooAction::START:
        return AstrOsStringUtils::stringFormat("%d,start%c", this->ch, '\n');
    case KangarooAction::HOME:
        return AstrOsStringUtils::stringFormat("%d,home%c", this->ch, '\n');
    case KangarooAction::SPEED:
        return AstrOsStringUtils::stringFormat("%d,s%d%c", this->ch, this->spd, '\n');
    case KangarooAction::POSITION:
    {
        if (SerialCommand::spd > 0)
        {
            return AstrOsStringUtils::stringFormat("%d,p%d s%d%c", this->ch, this->pos, this->spd, '\n');
        }
        else
        {
            return AstrOsStringUtils::stringFormat("%d,p%d%c", this->ch, this->pos, '\n');
        }
    }
    case KangarooAction::SPEED_INCREMENTAL:
        return AstrOsStringUtils::stringFormat("%d,si%d%c", this->ch, this->spd, '\n');
    case KangarooAction::POSITION_INCREMENTAL:
    {
        if (SerialCommand::spd > 0)
        {
            return AstrOsStringUtils::stringFormat("%d,pi%d s%d%c", this->ch, this->pos, this->spd, '\n');
        }
        else
        {
            return AstrOsStringUtils::stringFormat("%d,pi%d%c", this->ch, this->pos, '\n');
        }
    }
    default:
        return this->value;
    }
}