#ifndef ASTROSSTRINGUTILS_H
#define ASTROSSTRINGUTILS_H

#include <string>
#include <vector>
#include <cctype>
#include <algorithm>

class AstrOsStringUtils
{
public:
    static std::string macToString(const uint8_t *mac)
    {
        char macStr[18] = {0};
        snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return std::string(macStr);
    }

    static std::vector<std::string> splitString(std::string str, char delimiter)
    {
        std::vector<std::string> parts;
        auto start = 0U;
        auto end = str.find(delimiter);
        while (end != std::string::npos)
        {
            parts.push_back(str.substr(start, end - start));
            start = end + 1;
            end = str.find(delimiter, start);
        }

        parts.push_back(str.substr(start, end));

        return parts;
    }

    static bool caseInsensitiveCmp(std::string str1, std::string str2)
    {
        std::transform(str1.begin(), str1.end(), str1.begin(), ::tolower);
        std::transform(str2.begin(), str2.end(), str2.begin(), ::tolower);
        return str1 == str2;
    }
};

#endif