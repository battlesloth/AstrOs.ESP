#include <AstrOsSha256.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    std::string toHex(const uint8_t *bytes, size_t len)
    {
        static const char kHex[] = "0123456789abcdef";
        std::string out;
        out.resize(len * 2);
        for (size_t i = 0; i < len; ++i)
        {
            out[2 * i] = kHex[(bytes[i] >> 4) & 0xF];
            out[2 * i + 1] = kHex[bytes[i] & 0xF];
        }
        return out;
    }

    std::string sha256Hex(const uint8_t *data, size_t len)
    {
        AstrOsSha256Ctx ctx;
        AstrOsSha256_init(&ctx);
        AstrOsSha256_update(&ctx, data, len);
        uint8_t digest[ASTROS_SHA256_DIGEST_LEN];
        AstrOsSha256_final(&ctx, digest);
        return toHex(digest, sizeof(digest));
    }
} // namespace

// NIST FIPS-180-2 / RFC 6234 known-answer vectors.

TEST(Sha256, EmptyInput)
{
    EXPECT_EQ("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", sha256Hex(nullptr, 0));
}

TEST(Sha256, Abc)
{
    const auto *data = reinterpret_cast<const uint8_t *>("abc");
    EXPECT_EQ("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", sha256Hex(data, 3));
}

TEST(Sha256, TwoBlockBoundary448Bit)
{
    // Classic FIPS-180-2 example: 56-byte ASCII input forces the
    // length-padding into a second compression block.
    const char *msg = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    EXPECT_EQ("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
              sha256Hex(reinterpret_cast<const uint8_t *>(msg), 56));
}

TEST(Sha256, MillionA)
{
    // RFC 6234 third test vector: one million 'a' characters.
    std::vector<uint8_t> data(1'000'000, static_cast<uint8_t>('a'));
    EXPECT_EQ("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", sha256Hex(data.data(), data.size()));
}

TEST(Sha256, MultiUpdateMatchesSingleUpdate)
{
    // Same payload, split across many update calls of varying size.
    // The OTA receiver feeds chunks of up to 4096 bytes; this guards
    // the streaming path against an off-by-one in datalen handling
    // across calls.
    std::vector<uint8_t> data(10000);
    for (size_t i = 0; i < data.size(); ++i)
    {
        data[i] = static_cast<uint8_t>(i * 31 + 7);
    }
    const std::string singleShot = sha256Hex(data.data(), data.size());

    AstrOsSha256Ctx ctx;
    AstrOsSha256_init(&ctx);
    const size_t splits[] = {1, 63, 64, 65, 127, 128, 129, 1000, 4096};
    size_t offset = 0;
    size_t i = 0;
    while (offset < data.size())
    {
        size_t take = splits[i % (sizeof(splits) / sizeof(splits[0]))];
        if (offset + take > data.size())
        {
            take = data.size() - offset;
        }
        AstrOsSha256_update(&ctx, data.data() + offset, take);
        offset += take;
        ++i;
    }
    uint8_t digest[ASTROS_SHA256_DIGEST_LEN];
    AstrOsSha256_final(&ctx, digest);
    EXPECT_EQ(singleShot, toHex(digest, sizeof(digest)));
}

TEST(Sha256, ContextIsReusableAfterReinit)
{
    // After final + re-init the context must produce a fresh,
    // correct digest with no carryover from the previous run.
    AstrOsSha256Ctx ctx;
    AstrOsSha256_init(&ctx);
    const auto *first = reinterpret_cast<const uint8_t *>("hello");
    AstrOsSha256_update(&ctx, first, 5);
    uint8_t throwaway[ASTROS_SHA256_DIGEST_LEN];
    AstrOsSha256_final(&ctx, throwaway);

    AstrOsSha256_init(&ctx);
    AstrOsSha256_update(&ctx, reinterpret_cast<const uint8_t *>("abc"), 3);
    uint8_t digest[ASTROS_SHA256_DIGEST_LEN];
    AstrOsSha256_final(&ctx, digest);
    EXPECT_EQ("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", toHex(digest, sizeof(digest)));
}
