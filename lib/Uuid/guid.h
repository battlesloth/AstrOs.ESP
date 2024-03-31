#ifndef GUID_H
#define GUID_H

#include <esp_random.h>
#include <string>
#include <string.h>

namespace guid
{
    /// @brief this does not meet UUID spec, but is good enough for our purposes and is super memory friendly
    /// @return
    std::string generate_guid()
    {

        uint8_t raw[16];

        memset(raw, 0, 16);

        esp_fill_random(raw, 16);

        char buff[64];
        sprintf(buff, "%x%x%x%x-%x%x-%x%x-%x%x-%x%x%x%x%x%x", raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7], raw[8], raw[9], raw[10], raw[11], raw[12], raw[13], raw[14], raw[15]);
        return buff;
    }
}

#endif