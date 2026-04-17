/*
 * espnow_peer.h — Relocated from lib/AstrOsEspNow/src/AstrOsEspNowUtility.h
 * so both the ESP-IDF adapter (C++) and NvsManager.c (C) can share the
 * NVS wire-format struct from a pure, native-testable library.
 *
 * Layout is INTENTIONALLY preserved byte-for-byte (field order, names,
 * sizes) — NvsManager serialises / deserialises the struct directly, so
 * any rename or reorder would invalidate peer configs already persisted
 * on deployed devices.
 */

#ifndef ASTROS_ESPNOW_PEER_H
#define ASTROS_ESPNOW_PEER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define ESPNOW_ETH_MAC_LEN 6
#define ESPNOW_PEER_LIMIT 10

    typedef struct
    {
        int id;
        char name[16];
        uint8_t mac_addr[ESPNOW_ETH_MAC_LEN];
        char crypto_key[16];
        bool is_paired;
        bool pollAckThisCycle;
    } espnow_peer_t;

#ifdef __cplusplus
}
#endif

#endif
