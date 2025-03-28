#ifndef GPIOCOMMAND_HPP
#define GPIOCOMMAND_HPP

#include <BaseCommand.hpp>

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