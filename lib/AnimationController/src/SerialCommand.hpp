#ifndef SERIALCOMMAND_HPP
#define SERIALCOMMAND_HPP

#include <BaseCommand.hpp>

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
    int baudRate;
    std::string value;
};

#endif