#ifndef ANIMATIONCOMMAND_HPP
#define ANIMATIONCOMMAND_HPP

#include <AnimationCommon.hpp>

class CommandTemplate
{
public:
    CommandTemplate(AnimationCmdType type, int module, std::string val);
    ~CommandTemplate();
    AnimationCmdType type;
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

    AnimationCmdType commandType;
    std::string commandTemplate;
    int duration;
    // for most serial events, this will be the channel
    // for Maestro, this will be the module index
    int module;

    CommandTemplate *GetCommandTemplatePtr();
};

#endif