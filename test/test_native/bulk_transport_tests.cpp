#include <AstrOsBulkTransport.hpp>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace
{
    // Helper: compute CRC over a string literal (excluding null terminator).
    uint16_t crc(const char *s)
    {
        return AstrOsBulkTransport::crc16_ccitt_false(reinterpret_cast<const uint8_t *>(s), std::strlen(s));
    }
} // namespace

//=================================================================================================
// CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection, no XOR-out)
//=================================================================================================

TEST(BulkTransport, Crc16EmptyInputReturnsInitValue)
{
    // Empty input: CRC equals the init value 0xFFFF.
    EXPECT_EQ(0xFFFFu, AstrOsBulkTransport::crc16_ccitt_false(nullptr, 0));
}

TEST(BulkTransport, Crc16CanonicalCheckVector)
{
    // "123456789" -> 0x29B1 per the canonical CCITT-FALSE test vector.
    EXPECT_EQ(0x29B1u, crc("123456789"));
}

TEST(BulkTransport, Crc16SingleZeroByte)
{
    // A single 0x00 byte: well-known CCITT-FALSE result 0xE1F0.
    const uint8_t data[] = {0x00};
    EXPECT_EQ(0xE1F0u, AstrOsBulkTransport::crc16_ccitt_false(data, 1));
}

TEST(BulkTransport, Crc16SingleFfByte)
{
    // A single 0xFF byte: well-known CCITT-FALSE result 0xFF00.
    const uint8_t data[] = {0xFF};
    EXPECT_EQ(0xFF00u, AstrOsBulkTransport::crc16_ccitt_false(data, 1));
}

TEST(BulkTransport, Crc16DeterministicAcrossCalls)
{
    // Same input on two separate invocations must produce the same output —
    // a regression guard for any future caching/state mistake.
    const uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF};
    uint16_t first = AstrOsBulkTransport::crc16_ccitt_false(data, 4);
    uint16_t second = AstrOsBulkTransport::crc16_ccitt_false(data, 4);
    EXPECT_EQ(first, second);
}
