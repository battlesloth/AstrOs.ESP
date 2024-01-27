#include <gtest/gtest.h>
#include <AstrOsMessaging.h>

template <typename... Args>
std::string stringFormat(const std::string &format, Args &&...args)
{
    auto size = std::snprintf(nullptr, 0, format.c_str(), std::forward<Args>(args)...);
    std::string output(size + 1, '\0');
    std::sprintf(&output[0], format.c_str(), std::forward<Args>(args)...);
    return output;
}

TEST(AnimationCommand, ParseCommandType)
{
    std::string testString = "Registraion";
    auto values = AstrOsEspNowMessageParser::generatePackets(AstrOsPacketType::REGISTRATION, testString);
    auto parsed = AstrOsEspNowMessageParser::parsePacket(values[0]);

    auto message = stringFormat("value: %s", values[0]);

    for (size_t i = 0; i < values.size(); i++)
    {
        free(values[i]);
    }
}

int main(int argc, char **argv)
{
    testing::InitGoogleTest(&argc, argv);
    if (RUN_ALL_TESTS())
        ;

    return 0;
}

/*#include "unity.h"
#include <AstrOsMessaging.h>
#include <TestUtility.h>

void setUp(void)
{
    // set stuff up here
}

void tearDown(void)
{
    // clean stuff up here
}

void test_generate_parse_registration_packet(void)
{
    std::string testString = "Registraion";
    auto values = AstrOsEspNowMessageParser::generatePackets(AstrOsPacketType::REGISTRATION, testString);
    auto parsed = AstrOsEspNowMessageParser::parsePacket(values[0]);

    auto message = TestUtility::stringFormat("value: %s", values[0]);

    TEST_MESSAGE(message.c_str());

    TEST_ASSERT_EQUAL(1, values.size());
    TEST_ASSERT_EQUAL(AstrOsPacketType::REGISTRATION, parsed.packetType);
    TEST_ASSERT_EQUAL(1, parsed.packetNumber);
    TEST_ASSERT_EQUAL(1, parsed.totalPackets);
    TEST_ASSERT_EQUAL(1, parsed.packetType);
    TEST_ASSERT_EQUAL("Registration", parsed.payload);

    for (size_t i = 0; i < values.size(); i++)
    {
        free(values[i]);
    }
}

// not needed when using generate_test_runner.rb
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_generate_parse_registration_packet);
    return UNITY_END();
}*/