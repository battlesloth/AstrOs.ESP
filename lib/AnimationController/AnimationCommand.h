#ifndef ANIMATIONCOMMAND_H
#define ANIMATIONCOMMAND_H

#include <ServoCommand.h>
#include <string>
#include <vector>

typedef std::vector<std::string> str_vec_t;

enum CommandType{
    None,
    GenericSerial,
    Kangaroo,
    PWM,
    I2C
};

class BaseCommand
{
    public:
        BaseCommand();
        ~BaseCommand();
        CommandType commandType;
};

class AnimationCommand
{
private:
    std::string commandTemplate;
    void parseCommandType();
    str_vec_t splitTemplate();
public:
    AnimationCommand(std::string val);
    ~AnimationCommand();

    CommandType commandType;
    int duration;

    BaseCommand* toCommandPtr();
};

class GenericSerialCommand: public BaseCommand
{
    public:
        GenericSerialCommand();
        ~GenericSerialCommand();
};

class KangarooCommand: public BaseCommand 
{
    public:
        KangarooCommand();
        ~KangarooCommand();
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