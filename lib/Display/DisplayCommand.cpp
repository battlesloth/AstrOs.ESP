#include <DisplayCommand.h>
#include <sstream>
#include <string>
#include <vector>

#include <esp_log.h>


DisplayCommand::DisplayCommand()
{
    useLine1 = false;
    useLine2 = false;
    useLine3 = false;
    line1 = "";
    line2 = "";
    line3 = "";
}

DisplayCommand::DisplayCommand(const char *cmd)
{
    DisplayCommand::command = std::string(cmd);
    DisplayCommand::parseCommand();
}

DisplayCommand::~DisplayCommand()
{
}

void DisplayCommand::setLine(int line, std::string value){
    switch (line)
    {
    case 1:
        useLine1 = true;
        line1 = value;
        break;
    case 2:
        useLine2 = true;
        line2 = value;
        break;
    case 3:
        useLine3 = true;
        line3 = value;
        break;
    default:
        break;
    }
}

void DisplayCommand::parseCommand(){
    str_vec_t parts;

    auto start = 0U;
    auto end = DisplayCommand::command.find("|");
    while (end != std::string::npos)
    {
        parts.push_back(DisplayCommand::command.substr(start, end - start));
        start = end + 1;
        end = DisplayCommand::command.find("|", start);
    }

    parts.push_back(DisplayCommand::command.substr(start, end));

    DisplayCommand::useLine1 = parts.at(0) == "1";
    DisplayCommand::line1 = parts.at(1);
    DisplayCommand::useLine2 = parts.at(2) == "1";
    DisplayCommand::line2 = parts.at(3);
    DisplayCommand::useLine3 = parts.at(4) == "1";
    DisplayCommand::line3 = parts.at(5);
}

std::string DisplayCommand::toString(){
    std::stringstream ss;
    ss << (DisplayCommand::useLine1 ? "1": "0") << "|" << DisplayCommand::line1 << "|" 
       << (DisplayCommand::useLine2 ? "1": "0") << "|" << DisplayCommand::line2 << "|" 
       << (DisplayCommand::useLine3 ? "1": "0") << "|" << DisplayCommand::line3;

    return ss.str();
}
