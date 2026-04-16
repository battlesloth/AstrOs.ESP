#include <AstrOsPathUtils.hpp>
#include <gtest/gtest.h>

#include <string>

using AstrOsPathUtils::isPathSafe;
using AstrOsPathUtils::MAX_PATH_LEN;

TEST(PathUtils, AcceptsSimpleRelativePath)
{
    std::string reason = "preexisting";
    EXPECT_TRUE(isPathSafe("scripts/foo.scr", reason));
    EXPECT_TRUE(reason.empty());
}

TEST(PathUtils, AcceptsNestedRelativePath)
{
    std::string reason;
    EXPECT_TRUE(isPathSafe("scripts/sub/dir/payload.cfg", reason));
    EXPECT_TRUE(reason.empty());
}

TEST(PathUtils, RejectsEmptyPath)
{
    std::string reason;
    EXPECT_FALSE(isPathSafe("", reason));
    EXPECT_EQ(reason, "empty path rejected");
}

TEST(PathUtils, RejectsAbsolutePath)
{
    std::string reason;
    EXPECT_FALSE(isPathSafe("/etc/passwd", reason));
    EXPECT_EQ(reason, "absolute path rejected: /etc/passwd");
}

TEST(PathUtils, RejectsTraversalDotDot)
{
    std::string reason;
    EXPECT_FALSE(isPathSafe("../foo.scr", reason));
    EXPECT_EQ(reason, "traversal component rejected: ../foo.scr");
}

TEST(PathUtils, RejectsTraversalDotDotMidPath)
{
    std::string reason;
    EXPECT_FALSE(isPathSafe("scripts/../../etc/passwd", reason));
    EXPECT_EQ(reason, "traversal component rejected: scripts/../../etc/passwd");
}

TEST(PathUtils, RejectsDoubleSlash)
{
    std::string reason;
    EXPECT_FALSE(isPathSafe("scripts//foo.scr", reason));
    EXPECT_EQ(reason, "double-slash rejected: scripts//foo.scr");
}

TEST(PathUtils, RejectsOverlongPath)
{
    std::string longPath(MAX_PATH_LEN + 1, 'a');
    std::string reason;
    EXPECT_FALSE(isPathSafe(longPath, reason));

    // Truncation format: "path too long (N > MAX): <first-40-chars>..."
    std::string expectedPrefix =
        "path too long (" + std::to_string(longPath.size()) + " > " + std::to_string(MAX_PATH_LEN) + "): ";
    ASSERT_GE(reason.size(), expectedPrefix.size());
    EXPECT_EQ(reason.substr(0, expectedPrefix.size()), expectedPrefix);
    EXPECT_NE(reason.find("..."), std::string::npos);
}

TEST(PathUtils, AcceptsExactlyMaxPathLen)
{
    std::string boundaryPath(MAX_PATH_LEN, 'a');
    std::string reason = "leftover";
    EXPECT_TRUE(isPathSafe(boundaryPath, reason));
    EXPECT_TRUE(reason.empty());
}

TEST(PathUtils, EmptyTakesPrecedenceOverOtherChecks)
{
    // Ensures the guard order is consistent with the original implementation:
    // empty must be the first reject reason the caller sees.
    std::string reason = "stale";
    EXPECT_FALSE(isPathSafe("", reason));
    EXPECT_EQ(reason, "empty path rejected");
}
