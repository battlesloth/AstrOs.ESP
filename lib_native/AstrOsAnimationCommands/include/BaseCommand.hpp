#ifndef BASECOMMAND_HPP
#define BASECOMMAND_HPP

#include <AnimationCommon.hpp>
#include <AstrOsEnums.h>
#include <string>
#include <vector>

class BaseCommand
{
public:
    BaseCommand();
    virtual ~BaseCommand();
    str_vec_t SplitTemplate(std::string val);
    MODULE_TYPE type;
};

#endif
