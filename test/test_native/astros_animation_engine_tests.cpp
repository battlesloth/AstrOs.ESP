#include <AstrOsAnimationEngine.hpp>
#include <AstrOsEnums.h>
#include <gtest/gtest.h>

#include <string>

// ---------------- parseAnimationScript ----------------

TEST(AnimationEngine, ParseScriptSingleEvent)
{
    auto events = AstrOsAnimationEngine::parseAnimationScript("1|500|0|ctrl|3|75|100|50");
    ASSERT_EQ(1u, events.size());
    EXPECT_EQ(MODULE_TYPE::MAESTRO, events[0].commandType);
    EXPECT_EQ(500, events[0].duration);
}

TEST(AnimationEngine, ParseScriptMultipleEventsReversed)
{
    auto events = AstrOsAnimationEngine::parseAnimationScript("5|100|2|1|1;1|500|0|c|3|75|100|50;3|200|1|1|9600|hi");
    ASSERT_EQ(3u, events.size());
    // Reversed: last script event is first in the vector for back-to-front dispatch
    EXPECT_EQ(MODULE_TYPE::GENERIC_SERIAL, events[0].commandType);
    EXPECT_EQ(MODULE_TYPE::MAESTRO, events[1].commandType);
    EXPECT_EQ(MODULE_TYPE::GPIO, events[2].commandType);
}

TEST(AnimationEngine, ParseScriptSkipsEmptySegments)
{
    auto events = AstrOsAnimationEngine::parseAnimationScript("1|500|0|c|3|75|100|50;;5|100|2|1|1;");
    ASSERT_EQ(2u, events.size());
}

TEST(AnimationEngine, ParseScriptEmptyStringReturnsEmpty)
{
    auto events = AstrOsAnimationEngine::parseAnimationScript("");
    EXPECT_TRUE(events.empty());
}

// ---------------- getNextCommand ----------------

TEST(AnimationEngine, GetNextCommandFromEmptyEventsReturnsDone)
{
    std::vector<AnimationCommand> events;
    auto result = AstrOsAnimationEngine::getNextCommand(events);

    ASSERT_NE(nullptr, result.command);
    EXPECT_EQ(MODULE_TYPE::NONE, result.command->type);
    EXPECT_TRUE(result.scriptDone);
    EXPECT_EQ(0, result.delayMs);
}

TEST(AnimationEngine, GetNextCommandSingleEventReturnsDone)
{
    auto events = AstrOsAnimationEngine::parseAnimationScript("1|500|0|c|3|75|100|50");
    ASSERT_EQ(1u, events.size());

    auto result = AstrOsAnimationEngine::getNextCommand(events);

    EXPECT_EQ(MODULE_TYPE::MAESTRO, result.command->type);
    EXPECT_TRUE(result.scriptDone);
    EXPECT_EQ(0, result.delayMs);
    EXPECT_TRUE(events.empty());
}

TEST(AnimationEngine, GetNextCommandMultipleEventsReturnsDelay)
{
    auto events = AstrOsAnimationEngine::parseAnimationScript("5|100|2|1|1;1|500|0|c|3|75|100|50");
    ASSERT_EQ(2u, events.size());

    auto result = AstrOsAnimationEngine::getNextCommand(events);

    EXPECT_FALSE(result.scriptDone);
    EXPECT_EQ(1u, events.size());
    EXPECT_EQ(100, result.delayMs);
}

TEST(AnimationEngine, GetNextCommandMinimumDelay10ms)
{
    // Duration 5ms should be clamped to 10ms minimum
    auto events = AstrOsAnimationEngine::parseAnimationScript("5|5|2|1|1;1|500|0|c|3|75|100|50");

    auto result = AstrOsAnimationEngine::getNextCommand(events);

    EXPECT_FALSE(result.scriptDone);
    EXPECT_EQ(10, result.delayMs);
}

TEST(AnimationEngine, GetNextCommandDispatchesFullScript)
{
    auto events = AstrOsAnimationEngine::parseAnimationScript("5|100|2|1|1;1|500|0|c|3|75|100|50;3|200|1|1|9600|hi");
    ASSERT_EQ(3u, events.size());

    // First dispatch — 2 remaining
    auto r1 = AstrOsAnimationEngine::getNextCommand(events);
    EXPECT_FALSE(r1.scriptDone);
    EXPECT_EQ(2u, events.size());

    // Second dispatch — 1 remaining
    auto r2 = AstrOsAnimationEngine::getNextCommand(events);
    EXPECT_FALSE(r2.scriptDone);
    EXPECT_EQ(1u, events.size());

    // Third dispatch — done
    auto r3 = AstrOsAnimationEngine::getNextCommand(events);
    EXPECT_TRUE(r3.scriptDone);
    EXPECT_TRUE(events.empty());
}

// ---------------- ScriptQueue ----------------

TEST(AnimationEngine, ScriptQueuePushAndPop)
{
    ScriptQueue<5> q;
    EXPECT_TRUE(q.isEmpty());
    EXPECT_TRUE(q.push("script1"));
    EXPECT_TRUE(q.push("script2"));
    EXPECT_EQ(2, q.size());

    EXPECT_EQ("script1", q.pop());
    EXPECT_EQ("script2", q.pop());
    EXPECT_TRUE(q.isEmpty());
}

TEST(AnimationEngine, ScriptQueueFullRejectsPush)
{
    ScriptQueue<3> q;
    EXPECT_TRUE(q.push("a"));
    EXPECT_TRUE(q.push("b"));
    EXPECT_TRUE(q.push("c"));
    EXPECT_TRUE(q.isFull());
    EXPECT_FALSE(q.push("d"));
}

TEST(AnimationEngine, ScriptQueueWrapsAround)
{
    ScriptQueue<3> q;
    q.push("a");
    q.push("b");
    q.push("c");
    EXPECT_EQ("a", q.pop());
    // Now front=1, rear=2, size=2 — push should wrap rear to 0
    EXPECT_TRUE(q.push("d"));
    EXPECT_EQ("b", q.pop());
    EXPECT_EQ("c", q.pop());
    EXPECT_EQ("d", q.pop());
    EXPECT_TRUE(q.isEmpty());
}

TEST(AnimationEngine, ScriptQueueClear)
{
    ScriptQueue<5> q;
    q.push("a");
    q.push("b");
    q.clear();
    EXPECT_TRUE(q.isEmpty());
    EXPECT_EQ(0, q.size());
    // Should be usable again after clear
    EXPECT_TRUE(q.push("c"));
    EXPECT_EQ("c", q.pop());
}

TEST(AnimationEngine, ScriptQueuePopFromEmptyReturnsEmpty)
{
    ScriptQueue<3> q;
    EXPECT_EQ("", q.pop());
}
