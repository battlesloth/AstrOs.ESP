#include <AnimationCommand.h>
#include <string>
#include <vector>

AnimationCommand::AnimationCommand(std::string val)
{
    commandTemplate = val;
    AnimationCommand::parseCommandType();
}

AnimationCommand::~AnimationCommand()
{
}

BaseCommand* AnimationCommand::toCommandPtr()
{

    BaseCommand* cmd;

    switch (commandType)
    {
    case GenericSerial:
        cmd = new GenericSerialCommand;
        break;
    case Kangaroo:
        cmd = new KangarooCommand;
        break;
    case PWM:
        cmd = new PwmCommand;
        break;
    case I2C:
        cmd = new I2cCommand;
        break;
    default:
        cmd = new BaseCommand;
        break;
    }

    return cmd;
}

void AnimationCommand::parseCommandType()
{

    str_vec_t script = AnimationCommand::splitTemplate();
    commandType = static_cast<CommandType>(std::stoi(script.at(0)));
    duration = std::stoi(script.at(1));
}

str_vec_t AnimationCommand::splitTemplate()
{

    str_vec_t result;

    auto start = 0U;
    auto end = commandTemplate.find("|");
    while (end != std::string::npos)
    {
        result.push_back(commandTemplate.substr(start, end - start));
        start = end + 1;
        end = commandTemplate.find("|", start);
    }

    return result;
}

BaseCommand::BaseCommand() {}
BaseCommand::~BaseCommand() {}

GenericSerialCommand::GenericSerialCommand() {}
GenericSerialCommand::~GenericSerialCommand() {}

KangarooCommand::KangarooCommand() {}
KangarooCommand::~KangarooCommand() {}

PwmCommand::PwmCommand() {}
PwmCommand::~PwmCommand() {}

I2cCommand::I2cCommand() {}
I2cCommand::~I2cCommand() {}