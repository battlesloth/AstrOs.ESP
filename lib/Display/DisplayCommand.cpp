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

void DisplayCommand::setValue(std::string line1, std::string line2, std::string line3)
{
    DisplayCommand::useLine1 = true;
    DisplayCommand::line1 = line1;
    DisplayCommand::useLine2 = line2.length() > 0;
    DisplayCommand::line2 = line2;
    DisplayCommand::useLine3 = line3.length() > 0;
    DisplayCommand::line3 = line3;
}

void DisplayCommand::parseCommand()
{
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

std::string DisplayCommand::toString()
{
    std::stringstream ss;
    ss << (DisplayCommand::useLine1 ? "1" : "0") << "|" << DisplayCommand::line1 << "|"
       << (DisplayCommand::useLine2 ? "1" : "0") << "|" << DisplayCommand::line2 << "|"
       << (DisplayCommand::useLine3 ? "1" : "0") << "|" << DisplayCommand::line3;

    return ss.str();
}
