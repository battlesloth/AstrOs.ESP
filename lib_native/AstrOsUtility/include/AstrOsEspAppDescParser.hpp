#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace AstrOsEspAppDescParser
{
    struct Result
    {
        bool ok = false;
        std::string version;
        std::string error;
    };

    // Parse esp_app_desc_t from the leading bytes of an ESP32 firmware .bin.
    // Expects at least 80 bytes: 24 B image header + 8 B segment header +
    // 4 B app_desc magic + 4 B secure_version + 8 B reserved + 32 B version.
    //
    // Validates:
    //   buf[0] == 0xE9                 (esp_image_header magic)
    //   buf[32..35] == 32 54 CD AB     (esp_app_desc magic_word, little-endian 0xABCD5432)
    //
    // Reads version[32] starting at offset 48, treated as a null-terminated
    // ASCII string (truncated at first '\0' or at 32 bytes).
    Result parse(const uint8_t *buf, std::size_t len);
} // namespace AstrOsEspAppDescParser
