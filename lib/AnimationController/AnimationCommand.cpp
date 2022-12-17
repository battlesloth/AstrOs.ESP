#include <AnimationCommand.h>
#include <AnimationCommon.h>
#include <string>
#include <vector>

#include <esp_log.h>

static const char *TAG = "AnimationCommand";

CommandTemplate::CommandTemplate(CommandType type, std::string val):val(std::move(val)){
    CommandTemplate::type = type;
}

CommandTemplate::~CommandTemplate(){}

AnimationCommand::AnimationCommand(std::string val):commandTemplate(std::move(val))
{
    AnimationCommand::parseCommandType();
}

AnimationCommand::~AnimationCommand()
{
}

CommandTemplate* AnimationCommand::GetCommandTemplatePtr()
{
    CommandTemplate* ct = new CommandTemplate(commandType, commandTemplate);
    return ct;
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

template <typename ...Args>
std::string BaseCommand::stringFormat(const std::string& format, Args && ...args)
{
    auto size = std::snprintf(nullptr, 0, format.c_str(), std::forward<Args>(args)...);
    std::string output(size + 1, '\0');
    std::sprintf(&output[0], format.c_str(), std::forward<Args>(args)...);
    return output;
}

SerialCommand::SerialCommand(std::string val) {

    str_vec_t parts = SplitTemplate(val);

    type = static_cast<CommandType>(std::stoi(parts.at(0)));
    
    if (type == CommandType::Kangaroo){
        ch = std::stoi(parts.at(2));
        cmd = std::stoi(parts.at(3));
        spd = std::stoi(parts.at(4));
        pos = std::stoi(parts.at(5));
    } else {
        SerialCommand::value = parts.at(2);
    }
}

SerialCommand::SerialCommand(){}

SerialCommand::~SerialCommand() {}

std::string SerialCommand::GetValue(){

    if (type == CommandType::Kangaroo){
        return ToKangarooCommand();
    } else {
        return value;
    } 
}

std::string SerialCommand::ToKangarooCommand() {
    switch (cmd)
    {
    case KANGAROO_ACTION::START:
        return stringFormat("%d,start%c", ch, '\n');
    case KANGAROO_ACTION::HOME:
        return stringFormat("%d,home%c", ch, '\n');
    case KANGAROO_ACTION::SPEED:
        return stringFormat("%d,s%d%c", ch, spd, '\n');
    case KANGAROO_ACTION::POSITION:
        if (spd > 0){
            return stringFormat("%d,p%d s%d%c", ch, pos, spd, '\n');
        } else {
            return stringFormat("%d,p%d%c", ch, pos, '\n');
        }
    case KANGAROO_ACTION::SPEED_INCREMENTAL:
        return stringFormat("%d,si%d%c", ch, spd, '\n');
    case KANGAROO_ACTION::POSITION_INCREMENTAL:
        if (spd > 0){
            return stringFormat("%d,pi%d s%d%c", ch, pos, spd, '\n');
        } else {
            return stringFormat("%d,pi%d%c", ch, pos, '\n');
        }
    default:
        return value;
    }
}

ServoCommand::ServoCommand(std::string val) {

    str_vec_t parts = SplitTemplate(val);

    channel = std::stoi(parts.at(0));
    position = std::stoi(parts.at(1));
    speed = std::stoi(parts.at(2)); 
}
ServoCommand::~ServoCommand() {}

I2cCommand::I2cCommand() {}
I2cCommand::~I2cCommand() {}