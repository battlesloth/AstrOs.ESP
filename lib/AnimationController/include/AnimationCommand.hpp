#ifndef ANIMATIONCOMMAND_HPP
#define ANIMATIONCOMMAND_HPP

#include <string>
#include <vector>

typedef std::vector<std::string> str_vec_t;

enum CommandType
{
    None,
    PWM,
    I2C,
    GenericSerial,
    Kangaroo,
    Gpio
};

class CommandTemplate
{
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

    CommandTemplate *GetCommandTemplatePtr();

    int duration;
};

class BaseCommand
{
public:
    BaseCommand();
    virtual ~BaseCommand();
    str_vec_t SplitTemplate(std::string val);
    template <typename... Args>
    std::string stringFormat(const std::string &format, Args &&...args);
    CommandType type;
};

class SerialCommand : public BaseCommand
{
private:
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
    int serialChannel;
    std::string value;
};

class ServoCommand : public BaseCommand
{
private:
public:
    ServoCommand(std::string val);
    ~ServoCommand();
    int channel;
    int position;
    int speed;
    int acceleration;
};

class I2cCommand : public BaseCommand
{
private:
public:
    I2cCommand(std::string val);
    ~I2cCommand();
    int channel;
    std::string value;
};

class GpioCommand : public BaseCommand
{
private:
public:
    GpioCommand(std::string val);
    ~GpioCommand();
    int channel;
    bool state;
};

#endif