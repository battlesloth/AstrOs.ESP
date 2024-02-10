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
    auto value = AstrOsEspNowMessageService::generateEspNowMsg(AstrOsPacketType::POLL_ACK, "test", "macaddress");

    auto message = AstrOsStringUtils::getMessageValueAt(value.data, value.size, UNIT_SEPARATOR, 1);

    EXPECT_STREQ("test", message.c_str());
}