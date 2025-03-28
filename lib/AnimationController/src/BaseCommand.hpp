#ifndef BASECOMMAND_HPP
#define BASECOMMAND_HPP

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
    AnimationCmdType type;
};

#endif