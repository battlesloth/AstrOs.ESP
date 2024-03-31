#ifndef DISPLAYCOMMAND_HPP
#define DISPLAYCOMMAND_HPP

#include <string>
#include <vector>

typedef std::vector<std::string> str_vec_t;

class DisplayCommand
{
private:
    void parseCommand();
    std::string command;

public:
    DisplayCommand(std::string);
    DisplayCommand();
    ~DisplayCommand();
    void setValue(std::string line1, std::string line2 = "", std::string line3 = "");
    std::string toString();
    bool useLine1;
    bool useLine2;
    bool useLine3;
    std::string line1;
    std::string line2;
    std::string line3;
};

#endif