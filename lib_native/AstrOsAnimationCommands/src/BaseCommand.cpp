#include "BaseCommand.hpp"

BaseCommand::BaseCommand() {}
BaseCommand::~BaseCommand() {}

str_vec_t BaseCommand::SplitTemplate(std::string val)
{
    str_vec_t parts;

    auto start = 0U;
    auto end = val.find("|");
    while (end != std::string::npos)
    {
        parts.push_back(val.substr(start, end - start));
        start = end + 1;
        end = val.find("|", start);
    }

    parts.push_back(val.substr(start, end));

    return parts;
}
