#pragma once

#include <cstdint>

// On-wire byte layouts for ESP-NOW OTA frames (PR set 1).
//
// Direction: master → padawan = "downstream"; padawan → master = "upstream".
//
// All multi-byte integer fields are stored in native ESP32 byte order
// (little-endian). The only consumers of these structs are ESP32 nodes;
// no host-to-network conversion is required. __attribute__((packed))
// prevents the compiler from inserting alignment padding between fields,
// which would silently break wire compatibility.
//
// Each struct has a static_assert immediately after its definition that
// pins sizeof to the spec'd byte count. If any field type changes or the
// compiler inserts padding, the assertion fires at compile time.

// ─── Reason / status enums (wire-stable; values must not be renumbered) ──

enum class OtaBeginNakReason : uint8_t
{
    BUSY = 0,         // transfer already in flight
    NO_PARTITION = 1, // inactive partition missing or too small for totalSize
    BEGIN_FAILED = 2  // esp_ota_begin returned non-OK (MIXED layer only)
};

enum class OtaDataNakReason : uint8_t
{
    NONE = 0, // sentinel for uninitialized; NOT a valid wire value — the M1 Task 5
              // parser rejects records whose on-wire reason byte is 0
    CRC = 1,
    SIZE = 2,
    OUT_OF_ORDER = 3,
    WRITE = 4 // esp_ota_write failed (MIXED layer only)
};

enum class OtaEndStatus : uint8_t
{
    OK = 0,
    HASH_MISMATCH = 1, // streamed SHA mismatch OR read-back-rehash mismatch
    WRITE_ERROR = 2    // chunk count mismatch OR esp_ota_end failed
};

// ─── Downstream frames (master → padawan) ────────────────────────────────

struct __attribute__((packed)) OtaBeginPayload
{
    uint8_t xferId;
    uint32_t totalSize;
    uint16_t chunkSize;
    uint32_t totalChunks;
    uint8_t sha256Expected[32];
    uint8_t flags; // bit0=enable-psram-buffer (reserved for future use); 0 in PR set 1
};
static_assert(sizeof(OtaBeginPayload) == 44, "OtaBeginPayload must be 44 bytes on the wire");

// OTA_DATA payload = header + variable-length firmware bytes.
// The MIXED layer reads payloadLen bytes immediately after the header.
struct __attribute__((packed)) OtaDataHeader
{
    uint8_t xferId;
    uint32_t seq;
    uint16_t payloadLen;
    uint16_t crc16; // CRC-16/CCITT-FALSE over [xferId..end of firmware-bytes]
                    // (matches the AstrOsBulkTransport::crc16_ccitt_false function)
};
static_assert(sizeof(OtaDataHeader) == 9, "OtaDataHeader must be 9 bytes on the wire");

struct __attribute__((packed)) OtaEndPayload
{
    uint8_t xferId;
    uint32_t totalChunksSent;
    uint8_t sha256Final[32];
};
static_assert(sizeof(OtaEndPayload) == 37, "OtaEndPayload must be 37 bytes on the wire");

// ─── Upstream frames (padawan → master) ──────────────────────────────────

struct __attribute__((packed)) OtaBeginAckPayload
{
    uint8_t xferId;
};
static_assert(sizeof(OtaBeginAckPayload) == 1, "OtaBeginAckPayload must be 1 byte on the wire");

struct __attribute__((packed)) OtaBeginNakPayload
{
    uint8_t xferId;
    uint8_t reason; // OtaBeginNakReason value
};
static_assert(sizeof(OtaBeginNakPayload) == 2, "OtaBeginNakPayload must be 2 bytes on the wire");

struct __attribute__((packed)) OtaDataAckPayload
{
    uint8_t xferId;
    uint32_t highestContiguousSeq;
    uint32_t nextExpectedSeq;
    uint8_t windowRemaining;
};
static_assert(sizeof(OtaDataAckPayload) == 10, "OtaDataAckPayload must be 10 bytes on the wire");

struct __attribute__((packed)) OtaDataNakPayload
{
    uint8_t xferId;
    uint32_t highestContiguousSeq;
    uint32_t nextExpectedSeq;
    uint8_t windowRemaining;
    uint8_t reason; // OtaDataNakReason value
};
static_assert(sizeof(OtaDataNakPayload) == 11, "OtaDataNakPayload must be 11 bytes on the wire");

struct __attribute__((packed)) OtaEndAckPayload
{
    uint8_t xferId;
    uint8_t status; // OtaEndStatus value
    uint8_t sha256Computed[32];
};
static_assert(sizeof(OtaEndAckPayload) == 34, "OtaEndAckPayload must be 34 bytes on the wire");
