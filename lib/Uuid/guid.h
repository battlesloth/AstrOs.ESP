#ifndef GUID_H
#define GUID_H

#include <esp_random.h>
#include <string.h>
#include <string>

namespace guid
{
    /// @brief this does not meet UUID spec, but is good enough for our purposes and is super memory friendly
    /// @return
    std::string generate_guid()
    {

        uint8_t raw[16];

        memset(raw, 0, 16);

        esp_fill_random(raw, 16);

        // Format each byte as fixed 2-char hex (%02x) so the string is a
        // deterministic 1:1 mapping from the 16 raw bytes — otherwise single-digit
        // values like 0x01 0x23 would collide with 0x12 0x03 (both print as "123").
        char buff[64];
        snprintf(buff, sizeof(buff), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                 (unsigned)raw[0], (unsigned)raw[1], (unsigned)raw[2], (unsigned)raw[3], (unsigned)raw[4],
                 (unsigned)raw[5], (unsigned)raw[6], (unsigned)raw[7], (unsigned)raw[8], (unsigned)raw[9],
                 (unsigned)raw[10], (unsigned)raw[11], (unsigned)raw[12], (unsigned)raw[13], (unsigned)raw[14],
                 (unsigned)raw[15]);
        return buff;
    }
} // namespace guid

#endif