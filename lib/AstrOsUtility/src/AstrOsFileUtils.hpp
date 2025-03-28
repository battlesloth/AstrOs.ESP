#ifndef ASTROSFILEUTILS_H
#define ASTROSFILEUTILS_H

#include <string>
#include <vector>
#include <map>
#include <AstrOsStringUtils.hpp>
#include <AstrOsStructs.h>

class AstrOsFileUtils
{
public:
    static std::vector<maestro_config> parseMaestroConfig(const std::string maestroFile)
    {
        std::vector<maestro_config> configs;

        auto lines = AstrOsStringUtils::splitString(maestroFile, '\n');

        for (const auto &line : lines)
        {
            auto trimmedLine = line;
            trimmedLine.erase(std::remove(trimmedLine.begin(), trimmedLine.end(), ' '), trimmedLine.end()); // remove spaces
            if (trimmedLine.empty() || trimmedLine[0] == '#')                                               // skip empty lines and comments
            {
                continue;
            }

            auto parts = AstrOsStringUtils::splitString(trimmedLine, '|');
            if (parts.size() != 3) // must have 3 parts
            {
                continue;
            }

            maestro_config cfg;
            cfg.idx = std::stoi(parts[0]);
            cfg.uartChannel = std::stoi(parts[1]);
            cfg.baudrate = std::stoi(parts[2]);

            configs.push_back(cfg);
        }

        return configs;
    }

    static std::vector<servo_channel> parseServoConfig(const std::string servoFile)
    {
        std::vector<servo_channel> channels;

        auto cfigs = AstrOsStringUtils::splitString(servoFile, '|');

        for (const auto &cfig : cfigs)
        {
            auto parts = AstrOsStringUtils::splitString(cfig, ':');
            if (parts.size() != 7) // must have 7 parts
            {
                continue;
            }

            servo_channel ch;
            ch.id = std::stoi(parts[0]);
            ch.enabled = std::stoi(parts[1]);
            ch.isServo = std::stoi(parts[2]);
            ch.minPos = std::stoi(parts[3]);
            ch.maxPos = std::stoi(parts[4]);

            auto home = std::stoi(parts[5]);
            ch.home = ch.minPos > home ? ch.minPos : home;

            ch.currentPos = ch.minPos;
            ch.requestedPos = ch.minPos;
            ch.lastPos = ch.home;
            ch.speed = 1;
            ch.acceleration = 0;
            ch.inverted = std::stoi(parts[6]);
            ch.on = false;

            channels.emplace(channels.begin() + ch.id, ch);
        }

        return channels;
    }
};

#endif