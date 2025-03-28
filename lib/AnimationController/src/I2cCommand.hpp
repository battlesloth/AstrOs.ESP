#ifndef I2CCOMMAND_HPP
#define I2CCOMMAND_HPP

#include <BaseCommand.hpp>

class I2cCommand : public BaseCommand
{
private:
public:
    I2cCommand(std::string val);
    ~I2cCommand();
    int channel;
    std::string value;
};

#endif