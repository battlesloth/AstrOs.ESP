#include <AnimationCommand.h>
#include <string>
#include <vector>

#include <esp_log.h>

static const char *TAG = "AnimationCommand";

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
        cmd = new KangarooCommand(commandTemplate);
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

    ESP_LOGI(TAG, "Template: %s", commandTemplate.c_str());
    ESP_LOGI(TAG, "CommandType: %d", commandType);
    ESP_LOGI(TAG, "Duration: %d", duration);
    
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

str_vec_t BaseCommand::SplitTemplate(std::string val){
    
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

GenericSerialCommand::GenericSerialCommand() {}
GenericSerialCommand::~GenericSerialCommand() {}

KangarooCommand::KangarooCommand(std::string val) {

    str_vec_t parts = SplitTemplate(val);

    commandType = static_cast<CommandType>(std::stoi(parts.at(0)));
    ch = std::stoi(parts.at(2));
    cmd = std::stoi(parts.at(3));
    spd = std::stoi(parts.at(4));
    pos = std::stoi(parts.at(5));
}

KangarooCommand::~KangarooCommand() {}

PwmCommand::PwmCommand() {}
PwmCommand::~PwmCommand() {}

I2cCommand::I2cCommand() {}
I2cCommand::~I2cCommand() {}