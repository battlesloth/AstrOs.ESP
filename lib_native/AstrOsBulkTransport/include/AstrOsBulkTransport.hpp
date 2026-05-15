#pragma once

#include <cstddef>
#include <cstdint>

namespace AstrOsBulkTransport
{
    // Single-frame CRC-16/CCITT-FALSE. Poly 0x1021, init 0xFFFF, no input
    // reflection, no output reflection, no XOR-out. Byte-identical to
    // ESP-IDF's esp_crc16_le. Standalone (not a class method) so tests
    // can pin behavior independently of the receiver state machine.
    uint16_t crc16_ccitt_false(const uint8_t *data, size_t len);
} // namespace AstrOsBulkTransport
