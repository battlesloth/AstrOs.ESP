#include <AstrOsPathUtils.hpp>
#include <gtest/gtest.h>

#include <cstring>
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

// --- contentAddressedFirmwarePath ---

using AstrOsPathUtils::contentAddressedFirmwarePath;
using AstrOsPathUtils::FIRMWARE_HASH_PREFIX_LEN;
using AstrOsPathUtils::FIRMWARE_PATH_BUF_LEN;

TEST(ContentAddressedFirmwarePath, BuildsExpectedPathForFullSha256)
{
    // SHA-256("") happens to be a convenient 64-char vector to feed in.
    const char *hex = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    char out[FIRMWARE_PATH_BUF_LEN] = {0};
    EXPECT_TRUE(contentAddressedFirmwarePath(hex, out, sizeof(out)));
    EXPECT_STREQ("/sdcard/firmware/e3b0c44298fc1c14.bin", out);
}

TEST(ContentAddressedFirmwarePath, UsesExactlyFirst16CharsOfHash)
{
    // Construct a hash where chars 0..15 differ from 16..31 to prove the
    // truncation point is exactly at 16 and not "wherever NUL falls".
    const char *hex = "0123456789abcdef"
                      "ffffffffffffffff"
                      "ffffffffffffffff"
                      "ffffffffffffffff";
    char out[FIRMWARE_PATH_BUF_LEN] = {0};
    EXPECT_TRUE(contentAddressedFirmwarePath(hex, out, sizeof(out)));
    EXPECT_STREQ("/sdcard/firmware/0123456789abcdef.bin", out);
}

TEST(ContentAddressedFirmwarePath, AcceptsExactly16CharHash)
{
    const char *hex = "aaaabbbbccccdddd";
    ASSERT_EQ(FIRMWARE_HASH_PREFIX_LEN, strlen(hex));
    char out[FIRMWARE_PATH_BUF_LEN] = {0};
    EXPECT_TRUE(contentAddressedFirmwarePath(hex, out, sizeof(out)));
    EXPECT_STREQ("/sdcard/firmware/aaaabbbbccccdddd.bin", out);
}

TEST(ContentAddressedFirmwarePath, RejectsShortHash)
{
    // Locks down the 16-char minimum that prevents silent %.16s truncation.
    const char *hex = "aaaabbbbccccdd";
    ASSERT_LT(strlen(hex), FIRMWARE_HASH_PREFIX_LEN);
    char out[FIRMWARE_PATH_BUF_LEN] = {'X'};
    EXPECT_FALSE(contentAddressedFirmwarePath(hex, out, sizeof(out)));
    EXPECT_EQ('\0', out[0]);
}

TEST(ContentAddressedFirmwarePath, RejectsEmptyHash)
{
    char out[FIRMWARE_PATH_BUF_LEN] = {'X'};
    EXPECT_FALSE(contentAddressedFirmwarePath("", out, sizeof(out)));
    EXPECT_EQ('\0', out[0]);
}

TEST(ContentAddressedFirmwarePath, RejectsNullHash)
{
    char out[FIRMWARE_PATH_BUF_LEN] = {'X'};
    EXPECT_FALSE(contentAddressedFirmwarePath(nullptr, out, sizeof(out)));
    EXPECT_EQ('\0', out[0]);
}

TEST(ContentAddressedFirmwarePath, RejectsNullOut)
{
    EXPECT_FALSE(contentAddressedFirmwarePath("0123456789abcdef", nullptr, 64));
}

TEST(ContentAddressedFirmwarePath, RejectsUndersizedBuffer)
{
    // NUL on out[0] prevents the caller from consuming a truncated path.
    char out[FIRMWARE_PATH_BUF_LEN - 1] = {'X'};
    EXPECT_FALSE(contentAddressedFirmwarePath("0123456789abcdef", out, sizeof(out)));
    EXPECT_EQ('\0', out[0]);
}

TEST(ContentAddressedFirmwarePath, ZeroLengthBufferDoesNotWriteOut)
{
    // Reject must not touch the buffer — a stray out[0]='\0' would be a
    // 1-byte OOB write.
    char canary[2] = {'X', 'Y'};
    EXPECT_FALSE(contentAddressedFirmwarePath("0123456789abcdef", canary, 0));
    EXPECT_EQ('X', canary[0]);
    EXPECT_EQ('Y', canary[1]);
}

TEST(ContentAddressedFirmwarePath, IsDeterministic)
{
    // Same input must produce the same path.
    const char *hex = "feedfacecafebeef0011223344556677";
    char a[FIRMWARE_PATH_BUF_LEN] = {0};
    char b[FIRMWARE_PATH_BUF_LEN] = {0};
    EXPECT_TRUE(contentAddressedFirmwarePath(hex, a, sizeof(a)));
    EXPECT_TRUE(contentAddressedFirmwarePath(hex, b, sizeof(b)));
    EXPECT_STREQ(a, b);
}

TEST(ContentAddressedFirmwarePath, OutputIsNulTerminatedOnSuccess)
{
    const char *hex = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    char out[FIRMWARE_PATH_BUF_LEN] = {0};
    ASSERT_TRUE(contentAddressedFirmwarePath(hex, out, sizeof(out)));
    // strlen relies on the trailing NUL — if we lost it we'd run past
    // the buffer.
    EXPECT_EQ(strlen("/sdcard/firmware/") + 16 + 4, strlen(out));
}
