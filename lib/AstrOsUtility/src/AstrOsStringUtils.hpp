#ifndef ASTROSSTRINGUTILS_H
#define ASTROSSTRINGUTILS_H

#include <string>
#include <vector>
#include <cctype>
#include <algorithm>
#include <bitset>
#include <sstream>

#define UNIT_SEPARATOR (char)0x1F
#define RECORD_SEPARATOR (char)0x1E
#define GROUP_SEPARATOR (char)0x1D

class AstrOsStringUtils
{
public:
    template <typename T>
    static std::string toBinaryString(const T &x)
    {
        std::stringstream ss;
        ss << std::bitset<sizeof(T) * 8>(x);
        return ss.str();
    }

    static std::string macToString(const uint8_t *mac)
    {
        char macStr[18] = {0};
        snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return std::string(macStr);
    }

    static uint8_t *stringToMac(const std::string &macStr)
    {
        uint8_t *mac = new uint8_t[6];
        sscanf(macStr.c_str(), "%02hhX:%02hhX:%02hhX:%02hhX:%02hhX:%02hhX", &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]);
        return mac;
    }

    static std::string getMessageValueAt(uint8_t *data, int dataSize, char delimiter, int index)
    {
        std::string message = std::string(reinterpret_cast<char *>(data), dataSize);
        std::vector<std::string> parts = splitString(message, delimiter);
        if (index > parts.size() - 1)
        {
            return "";
        }

        return parts[index];
    }

    static std::vector<std::string> splitString(std::string str, char delimiter)
    {
        std::vector<std::string> parts;
        auto start = 0U;
        auto end = str.find(delimiter);

        if (end == std::string::npos)
        {
            parts.push_back(str);
            return parts;
        }

        while (end != std::string::npos)
        {
            parts.push_back(str.substr(start, end - start));
            start = end + 1;
            end = str.find(delimiter, start);
        }

        parts.push_back(str.substr(start, end));

        if (parts.back().empty())
        {
            parts.pop_back();
        }

        return parts;
    }

    /// @brief Files on Windows and Linux use different line endings, this function will split a string on both \n and \r\n
    /// @param str
    /// @return
    static std::vector<std::string> splitStringOnLineEnd(std::string str)
    {

        std::string search = "\r\n";
        std::string delimiter = "\n";

        size_t pos = str.find(search);
        while (pos != std::string::npos)
        {
            str.replace(pos, search.length(), delimiter);
            pos = str.find(search, pos + delimiter.length());
        }

        std::vector<std::string> parts;
        auto start = 0U;
        auto end = str.find(delimiter);

        if (end == std::string::npos)
        {
            parts.push_back(str);
            return parts;
        }

        while (end != std::string::npos)
        {
            parts.push_back(str.substr(start, end - start));
            start = end + delimiter.length();
            end = str.find(delimiter, start);
        }

        parts.push_back(str.substr(start, end));

        // if the last part is empty, remove it
        if (parts.back().empty())
        {
            parts.pop_back();
        }

        return parts;
    }

    static bool caseInsensitiveCmp(std::string str1, std::string str2)
    {
        std::transform(str1.begin(), str1.end(), str1.begin(), ::tolower);
        std::transform(str2.begin(), str2.end(), str2.begin(), ::tolower);
        return str1 == str2;
    }

    /// @brief Convert a string to an integer, string must be null terminated
    /// @param val char *
    /// @param size int
    /// @return int
    static int charToInt(char *val, int size)
    {
        int i, n;
        n = 0;
        for (i = 0; val[i] != '\0'; i++)
        {
            n = n * 10 + val[i] - '0';
        }

        return n;
    }

    template <typename... Args>
    static std::string stringFormat(const std::string &format, Args &&...args)
    {
        auto size = std::snprintf(nullptr, 0, format.c_str(), std::forward<Args>(args)...);
        std::string output(size + 1, '\0');
        std::sprintf(&output[0], format.c_str(), std::forward<Args>(args)...);
        return output;
    }
};

#endif