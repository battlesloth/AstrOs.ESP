#include "AstrOsBulkTransport.hpp"

namespace AstrOsBulkTransport
{
    uint16_t crc16_ccitt_false(const uint8_t *data, size_t len)
    {
        (void)data;
        (void)len;
        return 0; // implemented in Task 2
    }
} // namespace AstrOsBulkTransport
