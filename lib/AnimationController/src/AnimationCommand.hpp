#ifndef ANIMATIONCOMMAND_HPP
#define ANIMATIONCOMMAND_HPP

#include <AnimationCommon.hpp>
#include <AstrOsEnums.h>

class CommandTemplate
{
public:
    CommandTemplate(MODULE_TYPE type, int module, std::string val);
    ~CommandTemplate();
    MODULE_TYPE type;
    std::string val;
    int module;
};

class AnimationCommand
{
private:
    void parseCommandType();
    str_vec_t splitTemplate();

public:
    AnimationCommand(std::string val);
    ~AnimationCommand();

    MODULE_TYPE commandType;
    std::string commandTemplate;
    int duration;
    // for most serial events, this will be the channel
    // for Maestro, this will be the module index
    int module;

    CommandTemplate *GetCommandTemplatePtr();
};

#endif