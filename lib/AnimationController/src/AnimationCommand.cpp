#include <AnimationCommand.hpp>
#include <AnimationCommon.hpp>
#include <string>
#include <vector>
#include <esp_log.h>

CommandTemplate::CommandTemplate(AnimationCmdType type, int module, std::string val) : val(std::move(val))
{
    this->type = type;
    this->module = module;
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
    CommandTemplate *ct = new CommandTemplate(commandType, module, commandTemplate);
    return ct;
}

void AnimationCommand::parseCommandType()
{
    str_vec_t script = AnimationCommand::splitTemplate();
    AnimationCommand::commandType = static_cast<AnimationCmdType>(std::stoi(script.at(0)));
    AnimationCommand::duration = std::stoi(script.at(1));
    AnimationCommand::module = std::stoi(script.at(2));
}

str_vec_t AnimationCommand::splitTemplate()
{
    str_vec_t result;

    // we just need the first 3 spots
    auto start = 0U;
    auto end = commandTemplate.find("|");

    result.push_back(commandTemplate.substr(start, end - start));
    start = end + 1;
    end = commandTemplate.find("|", start);

    result.push_back(commandTemplate.substr(start, end));

    start = end + 1;
    end = commandTemplate.find("|", start);

    result.push_back(commandTemplate.substr(start, end));

    return result;
}
