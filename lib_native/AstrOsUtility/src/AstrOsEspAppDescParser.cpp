#include "AstrOsEspAppDescParser.hpp"

#include <cstring>

namespace AstrOsEspAppDescParser
{
    namespace
    {
        constexpr std::size_t kImageHeaderSize = 24;
        constexpr std::size_t kSegmentHeaderSize = 8;
        constexpr std::size_t kAppDescMagicOffset = kImageHeaderSize + kSegmentHeaderSize; // 32
        constexpr std::size_t kVersionOffset = kAppDescMagicOffset + 4 + 4 + 8;            // 48
        constexpr std::size_t kVersionMaxLen = 32;
        constexpr std::size_t kRequiredLen = kVersionOffset + kVersionMaxLen; // 80

        constexpr uint8_t kImageMagic = 0xE9;
        // 0xABCD5432 little-endian = 32 54 CD AB
        constexpr uint8_t kAppDescMagic[4] = {0x32, 0x54, 0xCD, 0xAB};
    } // namespace

    Result parse(const uint8_t *buf, std::size_t len)
    {
        Result r;
        if (buf == nullptr || len < kRequiredLen)
        {
            r.error = "truncated";
            return r;
        }
        if (buf[0] != kImageMagic)
        {
            r.error = "image_magic";
            return r;
        }
        if (std::memcmp(buf + kAppDescMagicOffset, kAppDescMagic, 4) != 0)
        {
            r.error = "app_desc_magic";
            return r;
        }

        const char *versionField = reinterpret_cast<const char *>(buf + kVersionOffset);
        std::size_t versionLen = ::strnlen(versionField, kVersionMaxLen);
        r.version.assign(versionField, versionLen);
        r.ok = true;
        return r;
    }
} // namespace AstrOsEspAppDescParser
