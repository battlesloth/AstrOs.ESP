#include "AstrOsEspAppDescParser.hpp"
#include <array>
#include <cstring>
#include <gtest/gtest.h>

namespace
{
    // Minimal synthetic .bin prefix: image header (1st byte = 0xE9, rest zeroed),
    // segment header (all zero), app desc (magic + secure_ver + 8B reserved + version).
    // Total prefix size needed: 24 + 8 + 4 + 4 + 8 + 32 = 80 bytes.
    constexpr std::size_t kPrefixSize = 80;

    std::array<uint8_t, kPrefixSize> makeValidPrefix(const char *version)
    {
        std::array<uint8_t, kPrefixSize> buf{};
        buf[0] = 0xE9; // esp_image_header_t magic
        // segment header (offset 24) — content unused
        // app desc magic at offset 32, little-endian 0xABCD5432
        buf[32] = 0x32;
        buf[33] = 0x54;
        buf[34] = 0xCD;
        buf[35] = 0xAB;
        // version[32] starts at offset 32 + 4 (magic) + 4 (secure_ver) + 8 (reserved) = 48
        std::memcpy(buf.data() + 48, version, std::strlen(version));
        return buf;
    }
} // namespace

TEST(EspAppDescParser, ParsesValidVersion)
{
    auto buf = makeValidPrefix("1.2.3-test");
    auto result = AstrOsEspAppDescParser::parse(buf.data(), buf.size());
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.version, "1.2.3-test");
    EXPECT_TRUE(result.error.empty());
}

TEST(EspAppDescParser, RejectsBadImageMagic)
{
    auto buf = makeValidPrefix("1.0.0");
    buf[0] = 0x00; // corrupt esp_image_header magic
    auto result = AstrOsEspAppDescParser::parse(buf.data(), buf.size());
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.version.empty());
    EXPECT_NE(result.error.find("image_magic"), std::string::npos);
}

TEST(EspAppDescParser, RejectsBadAppDescMagic)
{
    auto buf = makeValidPrefix("1.0.0");
    buf[32] = 0x00; // corrupt app_desc magic
    auto result = AstrOsEspAppDescParser::parse(buf.data(), buf.size());
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("app_desc_magic"), std::string::npos);
}

TEST(EspAppDescParser, RejectsTruncatedInput)
{
    auto buf = makeValidPrefix("1.0.0");
    auto result = AstrOsEspAppDescParser::parse(buf.data(), 40); // less than 80
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("truncated"), std::string::npos);
}

TEST(EspAppDescParser, HandlesEmptyVersionString)
{
    auto buf = makeValidPrefix("");
    auto result = AstrOsEspAppDescParser::parse(buf.data(), buf.size());
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.version, "");
}
