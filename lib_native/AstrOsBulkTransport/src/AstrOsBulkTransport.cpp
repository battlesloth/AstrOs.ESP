#include "AstrOsBulkTransport.hpp"

namespace AstrOsBulkTransport
{
    // CRC-16/CCITT-FALSE. Bit-by-bit reference implementation:
    //   poly = 0x1021, init = 0xFFFF, refIn = false, refOut = false, xorOut = 0.
    // Table-based variants would be faster but the volume here (one chunk =
    // ~4 KB per frame, ~5 MB/s peak throughput) doesn't justify the static
    // table cost in a PURE lib that has no on-device performance pressure.
    uint16_t crc16_ccitt_false(const uint8_t *data, size_t len)
    {
        uint16_t crc = 0xFFFFu;
        for (size_t i = 0; i < len; i++)
        {
            crc ^= static_cast<uint16_t>(data[i]) << 8;
            for (int bit = 0; bit < 8; bit++)
            {
                if (crc & 0x8000u)
                {
                    crc = static_cast<uint16_t>((crc << 1) ^ 0x1021u);
                }
                else
                {
                    crc = static_cast<uint16_t>(crc << 1);
                }
            }
        }
        return crc;
    }
} // namespace AstrOsBulkTransport
