#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <AstrOsStringUtils.hpp>
#include <AstrOsMessaging.hpp>

#define UNIT_SEPARATOR (char)0x1F

TEST(StringUtils, SplitString)
{
    std::stringstream testString;

    testString << "PART1" << UNIT_SEPARATOR << "PART2" << UNIT_SEPARATOR << "PART3";

    auto parts = AstrOsStringUtils::splitString(testString.str(), UNIT_SEPARATOR);

    EXPECT_EQ(3, parts.size());
    EXPECT_STREQ("PART1", parts[0].c_str());
    EXPECT_STREQ("PART2", parts[1].c_str());
    EXPECT_STREQ("PART3", parts[2].c_str());
}

TEST(StringUtils, GetMessageAt)
{
    auto msgService = AstrOsEspNowMessageService();
    auto value = msgService.generateEspNowMsg(AstrOsPacketType::POLL_ACK, "test", "macaddress")[0];

    auto message = AstrOsStringUtils::getMessageValueAt(value.data, value.size, UNIT_SEPARATOR, 1);

    EXPECT_STREQ("test", message.c_str());
}

TEST(StringUtils, splitStringOnLineEnd)
{
    std::string testString = "PART1\nPART2\r\nPART3\n";

    auto parts = AstrOsStringUtils::splitStringOnLineEnd(testString);

    EXPECT_EQ(3, parts.size());
    EXPECT_STREQ("PART1", parts[0].c_str());
    EXPECT_STREQ("PART2", parts[1].c_str());
    EXPECT_STREQ("PART3", parts[2].c_str());
}

TEST(StringUtils, splitStringOnLineEndWithEmptyLines)
{
    std::string testString = "PART1\n\nPART2\r\n\r\nPART3\n";

    auto parts = AstrOsStringUtils::splitStringOnLineEnd(testString);

    EXPECT_EQ(5, parts.size());
    EXPECT_STREQ("PART1", parts[0].c_str());
    EXPECT_STREQ("", parts[1].c_str());
    EXPECT_STREQ("PART2", parts[2].c_str());
    EXPECT_STREQ("", parts[3].c_str());
    EXPECT_STREQ("PART3", parts[4].c_str());
}

TEST(StringUtils, splitStringWithWindowsLineEndings)
{
    std::string testString = "PART1\r\nPART2\r\nPART3\r\n";

    auto parts = AstrOsStringUtils::splitStringOnLineEnd(testString);

    EXPECT_EQ(3, parts.size());
    EXPECT_STREQ("PART1", parts[0].c_str());
    EXPECT_STREQ("PART2", parts[1].c_str());
    EXPECT_STREQ("PART3", parts[2].c_str());
}