#ifndef ASTROS_SHA256_H
#define ASTROS_SHA256_H

// Software SHA-256 used by the OTA receiver path. The ESP-IDF
// mbedtls integration routes through the SoC's shared hardware SHA
// engine when CONFIG_MBEDTLS_HARDWARE_SHA=y; concurrent users of
// that engine elsewhere in the firmware can silently corrupt our
// streaming digest. This implementation runs entirely in software
// so the OTA hash is isolated from any other SHA consumer.
//
// Implementation lives in AstrOsSha256.c (Brad Conte, public domain).
// Pure C — no ESP-IDF/FreeRTOS includes — so it builds under the
// native test env and on both boards from one source.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define ASTROS_SHA256_DIGEST_LEN 32

    typedef struct
    {
        uint8_t data[64];
        uint32_t datalen;
        uint64_t bitlen;
        uint32_t state[8];
    } AstrOsSha256Ctx;

    // No matching free needed — the context owns no heap and is safe
    // to discard at any point.
    void AstrOsSha256_init(AstrOsSha256Ctx *ctx);

    void AstrOsSha256_update(AstrOsSha256Ctx *ctx, const uint8_t *data, size_t len);

    // Context must be re-initialised before reuse.
    void AstrOsSha256_final(AstrOsSha256Ctx *ctx, uint8_t hash[ASTROS_SHA256_DIGEST_LEN]);

#ifdef __cplusplus
}
#endif

#endif
