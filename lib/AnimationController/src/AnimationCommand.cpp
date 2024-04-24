#include <AnimationCommand.hpp>
#include <AnimationCommon.hpp>
#include <string>
#include <vector>
#include <esp_log.h>

CommandTemplate::CommandTemplate(CommandType type, std::string val) : val(std::move(val))
{
    CommandTemplate::type = type;
}

CommandTemplate::~CommandTemplate() {}

AnimationCommand::AnimationCommand(std::string val) : commandTemplate(std::move(val))
{
    AnimationCommand::parseCommandType();
}

AnimationCommand::~AnimationCommand()
{
}

CommandTemplate *AnimationCommand::GetCommandTemplatePtr()
{
    CommandTemplate *ct = new CommandTemplate(commandType, commandTemplate);
    return ct;
}

void AnimationCommand::parseCommandType()
{
    str_vec_t script = AnimationCommand::splitTemplate();
    AnimationCommand::commandType = static_cast<CommandType>(std::stoi(script.at(0)));
    AnimationCommand::duration = std::stoi(script.at(1));
}

str_vec_t AnimationCommand::splitTemplate()
{
    str_vec_t result;

    // we just need the first 2 spots
    auto start = 0U;
    auto end = commandTemplate.find("|");

    result.push_back(commandTemplate.substr(start, end - start));
    start = end + 1;
    end = commandTemplate.find("|", start);

    result.push_back(commandTemplate.substr(start, end));

    return result;
}

BaseCommand::BaseCommand() {}
BaseCommand::~BaseCommand() {}

str_vec_t BaseCommand::SplitTemplate(std::string val)
{

    str_vec_t parts;

    auto start = 0U;
    auto end = val.find("|");
    while (end != std::string::npos)
    {
        parts.push_back(val.substr(start, end - start));
        start = end + 1;
        end = val.find("|", start);
    }

    parts.push_back(val.substr(start, end));

    return parts;
}

template <typename... Args>
std::string BaseCommand::stringFormat(const std::string &format, Args &&...args)
{
    auto size = std::snprintf(nullptr, 0, format.c_str(), std::forward<Args>(args)...);
    std::string output(size + 1, '\0');
    std::sprintf(&output[0], format.c_str(), std::forward<Args>(args)...);
    return output;
}

SerialCommand::SerialCommand(std::string val)
{

    str_vec_t parts = SplitTemplate(val);

    if (parts.size() < 4)
    {
        ESP_LOGE("SerialCommand", "Invalid number of parts in command: %s", val.c_str());
        SerialCommand::type = CommandType::None;
        SerialCommand::serialChannel = -1;
        SerialCommand::ch = -1;
        SerialCommand::cmd = -1;
        SerialCommand::spd = -1;
        SerialCommand::pos = -1;
        SerialCommand::value = "";
        return;
    }

    SerialCommand::type = static_cast<CommandType>(std::stoi(parts.at(0)));

    SerialCommand::serialChannel = std::stoi(parts.at(2));

    if (SerialCommand::type == CommandType::Kangaroo)
    {
        SerialCommand::ch = std::stoi(parts.at(3));
        SerialCommand::cmd = std::stoi(parts.at(4));
        SerialCommand::spd = std::stoi(parts.at(5));
        SerialCommand::pos = std::stoi(parts.at(6));
    }
    else
    {
        SerialCommand::value = parts.at(3);
    }
}

SerialCommand::SerialCommand() {}

SerialCommand::~SerialCommand() {}

std::string SerialCommand::GetValue()
{

    if (SerialCommand::type == CommandType::Kangaroo)
    {
        return ToKangarooCommand();
    }
    else
    {
        return SerialCommand::value;
    }
}

std::string SerialCommand::ToKangarooCommand()
{
    switch (SerialCommand::cmd)
    {
    case KANGAROO_ACTION::START:
        return stringFormat("%d,start%c", SerialCommand::ch, '\n');
    case KANGAROO_ACTION::HOME:
        return stringFormat("%d,home%c", SerialCommand::ch, '\n');
    case KANGAROO_ACTION::SPEED:
        return stringFormat("%d,s%d%c", SerialCommand::ch, SerialCommand::spd, '\n');
    case KANGAROO_ACTION::POSITION:
        if (SerialCommand::spd > 0)
        {
            return stringFormat("%d,p%d s%d%c", SerialCommand::ch, SerialCommand::pos, SerialCommand::spd, '\n');
        }
        else
        {
            return stringFormat("%d,p%d%c", SerialCommand::ch, SerialCommand::pos, '\n');
        }
    case KANGAROO_ACTION::SPEED_INCREMENTAL:
        return stringFormat("%d,si%d%c", SerialCommand::ch, SerialCommand::spd, '\n');
    case KANGAROO_ACTION::POSITION_INCREMENTAL:
        if (SerialCommand::spd > 0)
        {
            return stringFormat("%d,pi%d s%d%c", SerialCommand::ch, SerialCommand::pos, SerialCommand::spd, '\n');
        }
        else
        {
            return stringFormat("%d,pi%d%c", SerialCommand::ch, SerialCommand::pos, '\n');
        }
    default:
        return SerialCommand::value;
    }
}

ServoCommand::ServoCommand(std::string val)
{
    str_vec_t parts = SplitTemplate(val);

    if (parts.size() < 5)
    {
        ESP_LOGE("ServoCommand", "Invalid number of parts in command: %s", val.c_str());
        ServoCommand::channel = -1;
        ServoCommand::position = -1;
        ServoCommand::speed = -1;
        return;
    }

    ServoCommand::channel = std::stoi(parts.at(2));
    ServoCommand::position = std::stoi(parts.at(3));
    ServoCommand::speed = std::stoi(parts.at(4));
}

ServoCommand::~ServoCommand() {}

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

GpioCommand::GpioCommand(std::string val)
{
    str_vec_t parts = SplitTemplate(val);

    if (parts.size() < 4)
    {
        ESP_LOGE("GpioCommand", "Invalid number of parts in command: %s", val.c_str());
        GpioCommand::channel = -1;
        GpioCommand::state = false;
        return;
    }

    GpioCommand::channel = std::stoi(parts.at(2));
    GpioCommand::state = std::stoi(parts.at(3));
}

GpioCommand::~GpioCommand() {}