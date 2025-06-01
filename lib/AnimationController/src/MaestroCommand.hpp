#ifndef MAESTROCOMMAND_HPP
#define MAESTROCOMMAND_HPP

#include <BaseCommand.hpp>

class MaestroCommand : public BaseCommand
{
private:
public:
    MaestroCommand(std::string val);
    ~MaestroCommand();
    std::string controller;
    int channel;
    int position;
    int speed;
    int acceleration;
};

#endif