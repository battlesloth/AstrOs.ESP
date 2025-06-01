#ifndef BASECOMMAND_HPP
#define BASECOMMAND_HPP

#include <AstrOsEnums.h>
#include <AnimationCommon.hpp>
#include <string>
#include <vector>

typedef std::vector<std::string> str_vec_t;

class BaseCommand
{
public:
    BaseCommand();
    virtual ~BaseCommand();
    str_vec_t SplitTemplate(std::string val);
    MODULE_TYPE type;
};

#endif