#ifndef ANIMATIONCOMMAND_H
#define ANIMATIONCOMMAND_H

#include <ServoCommand.h>
#include <string>
#include <vector>

typedef std::vector<std::string> str_vec_t;

enum CommandType{
    None,
    PWM,
    I2C,
    GenericSerial,
    Kangaroo
};

class CommandTemplate{
    public:
        CommandTemplate(CommandType type, std::string val);
        ~CommandTemplate();
        CommandType type;
        std::string val;
};

class AnimationCommand
{
private:
    void parseCommandType();
    str_vec_t splitTemplate();
public:
    AnimationCommand(std::string val);
    ~AnimationCommand();

    CommandType commandType;
    std::string commandTemplate;

    CommandTemplate* GetCommandTemplatePtr();

    int duration;
};


class BaseCommand
{
    public:
        BaseCommand();
        virtual ~BaseCommand();
        str_vec_t SplitTemplate(std::string val);
        template <typename ...Args>
        std::string stringFormat(const std::string& format, Args && ...args);
        CommandType type;
};

class SerialCommand: public BaseCommand
{
    private:
        std::string value;
        int ch;
        int cmd;
        int spd;
        int pos;
        std::string ToKangarooCommand();
    public:
        SerialCommand(std::string val);
        SerialCommand();
        ~SerialCommand();
        std::string GetValue();
};

class PwmCommand: public BaseCommand
{
    public:
        PwmCommand();
        ~PwmCommand();
};

class I2cCommand: public BaseCommand
{
    public:
        I2cCommand();
        ~I2cCommand();
};


#endif