#ifndef DISPLAYCOMMAND_H
#define DISPLAYCOMMAND_H

#include <string>
#include <vector>

typedef std::vector<std::string> str_vec_t;

class DisplayCommand{
    private:
        void parseCommand();
        std::string command;
    public:
        DisplayCommand(const char *cmd);
        DisplayCommand();
        ~DisplayCommand();
        void setLine(int line, std::string value);
        std::string toString();
        bool useLine1;
        bool useLine2;
        bool useLine3;
        std::string line1;
        std::string line2;
        std::string line3;
};

#endif