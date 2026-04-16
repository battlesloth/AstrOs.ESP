#include "AnimationCommand.hpp"

#include <cstdlib>
#include <string>
#include <vector>

CommandTemplate::CommandTemplate(MODULE_TYPE type, int module, std::string val) : val(std::move(val))
{
    this->type = type;
    this->module = module;
}

CommandTemplate::~CommandTemplate() {}

AnimationCommand::AnimationCommand(std::string val) : commandTemplate(std::move(val))
{
    this->parseCommandType();
}

AnimationCommand::~AnimationCommand() {}

std::unique_ptr<CommandTemplate> AnimationCommand::GetCommandTemplatePtr()
{
    return std::make_unique<CommandTemplate>(commandType, module, commandTemplate);
}

void AnimationCommand::parseCommandType()
{
    str_vec_t script = AnimationCommand::splitTemplate();

    if (script.size() < 3)
    {
        this->commandType = MODULE_TYPE::NONE;
        this->duration = 0;
        this->module = 0;
        return;
    }

    char *end = nullptr;

    long typeVal = std::strtol(script[0].c_str(), &end, 10);
    this->commandType = (end != script[0].c_str()) ? static_cast<MODULE_TYPE>(typeVal) : MODULE_TYPE::NONE;

    long durVal = std::strtol(script[1].c_str(), &end, 10);
    this->duration = (end != script[1].c_str()) ? static_cast<int>(durVal) : 0;

    long modVal = std::strtol(script[2].c_str(), &end, 10);
    this->module = (end != script[2].c_str()) ? static_cast<int>(modVal) : 0;
}

str_vec_t AnimationCommand::splitTemplate()
{
    str_vec_t result;

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
