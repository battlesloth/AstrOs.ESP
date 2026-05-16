#ifndef ASTROSSTRINGUTILS_H
#define ASTROSSTRINGUTILS_H

#include <algorithm>
#include <bitset>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#define UNIT_SEPARATOR (char)0x1F
#define RECORD_SEPARATOR (char)0x1E
#define GROUP_SEPARATOR (char)0x1D

class AstrOsStringUtils
{
public:
    template <typename T> static std::string toBinaryString(const T &x)
    {
        std::stringstream ss;
        ss << std::bitset<sizeof(T) * 8>(x);
        return ss.str();
    }

    static std::string macToString(const uint8_t *mac)
    {
        char macStr[18] = {0};
        snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4],
                 mac[5]);
        return std::string(macStr);
    }

    static uint8_t *stringToMac(const std::string &macStr)
    {
        uint8_t *mac = new uint8_t[6];
        sscanf(macStr.c_str(), "%02hhX:%02hhX:%02hhX:%02hhX:%02hhX:%02hhX", &mac[0], &mac[1], &mac[2], &mac[3], &mac[4],
               &mac[5]);
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

    /// @brief Files on Windows and Linux use different line endings, this function will split a string on both \n and
    /// \r\n
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

    /// @brief Parse a decimal string strictly into a uint8_t (0..255).
    ///
    /// Returns an empty optional on:
    ///   - empty input
    ///   - any non-digit character (including leading sign, leading/trailing whitespace, hex prefix)
    ///   - values > 255
    ///
    /// Note: strtoul accepts leading whitespace and a leading "+" / "-" by default, so this helper
    /// rejects them explicitly to keep the wire-protocol contract tight. Used by the OTA receive
    /// path to convert wire-level string transferId to BulkReceiver's uint8_t xferId; centralized
    /// here so the three OtaReceiver handlers don't each open-code the strtoul+errno+range dance.
    static std::optional<uint8_t> parseStrictU8(const std::string &s)
    {
        if (s.empty())
        {
            return std::nullopt;
        }
        for (char c : s)
        {
            if (c < '0' || c > '9')
            {
                return std::nullopt;
            }
        }
        errno = 0;
        char *endp = nullptr;
        unsigned long ul = std::strtoul(s.c_str(), &endp, 10);
        if (errno != 0 || endp == s.c_str() || *endp != '\0' || ul > 255)
        {
            return std::nullopt;
        }
        return static_cast<uint8_t>(ul);
    }

    template <typename... Args> static std::string stringFormat(const std::string &format, Args &&...args)
    {
        int size = std::snprintf(nullptr, 0, format.c_str(), std::forward<Args>(args)...);
        if (size < 0)
        {
            return std::string();
        }
        std::string output(static_cast<size_t>(size) + 1, '\0');
        std::snprintf(&output[0], static_cast<size_t>(size) + 1, format.c_str(), std::forward<Args>(args)...);
        output.resize(static_cast<size_t>(size));
        return output;
    }
};

#endif