#pragma once

#include <cstddef>
#include <string>

namespace AstrOsPathUtils
{
    // Maximum characters allowed in a user-supplied relative path. Matches
    // the previous inline constant inside AstrOsStorageManager::isPathSafe
    // so the QA-documented reject behaviour does not shift with this
    // extraction.
    constexpr std::size_t MAX_PATH_LEN = 128;

    // Returns true when `path` is safe to join against the storage mount
    // point. Rejects empty paths, absolute paths, any substring `..` or
    // `//`, and paths longer than MAX_PATH_LEN bytes.
    //
    // On rejection, writes a human-readable reason into `reasonOut`
    // formatted the way `AstrOsStorageManager` used to emit it inline,
    // e.g. "absolute path rejected: /foo" or "path too long (180 > 128):
    // <first-40-chars>...". Callers are expected to log it at
    // ESP_LOGW level at the boundary.
    //
    // On success `reasonOut` is cleared.
    bool isPathSafe(const std::string &path, std::string &reasonOut);

    // Content-addressed firmware path construction.
    //
    // Each verified firmware blob on SD is named after the first 16 hex chars
    // of its SHA-256 digest, under /sdcard/firmware/. The 16-char prefix has
    // ~1 in 2^64 collision odds and keeps the resulting path well within FAT
    // filename/path length limits.

    // Number of hex characters from the digest used to name the file.
    constexpr std::size_t FIRMWARE_HASH_PREFIX_LEN = 16;

    // Firmware tree root — keep these literals in sync. Both must share the
    // /sdcard/firmware/ prefix; the build won't catch drift between them, so
    // any change to one requires the matching change to the other.

    // Directory under which all verified firmware blobs are stored.
    constexpr const char *FIRMWARE_DIR = "/sdcard/firmware/";

    // In-progress OTA write target. Renamed to the content-addressed final
    // name on successful END+verify.
    constexpr const char *FIRMWARE_STAGING_PATH = "/sdcard/firmware/staging.bin";

    // Minimum out-buffer size for contentAddressedFirmwarePath including NUL.
    // Derived so any change to FIRMWARE_DIR or FIRMWARE_HASH_PREFIX_LEN is
    // picked up automatically. sizeof(".bin") is 5 (4 chars + NUL).
    constexpr std::size_t FIRMWARE_PATH_BUF_LEN =
        std::char_traits<char>::length(FIRMWARE_DIR) + FIRMWARE_HASH_PREFIX_LEN + sizeof(".bin");

    // Builds "/sdcard/firmware/<first-16-of-hashHex>.bin" into `out`.
    // Returns true on success. Returns false if:
    //   - `hashHex` is null or has fewer than FIRMWARE_HASH_PREFIX_LEN chars
    //     before its terminating NUL
    //   - `out` is null
    //   - `outLen` is less than FIRMWARE_PATH_BUF_LEN
    // On any failure where `out` and `outLen` are valid, `out[0]` is set to
    // NUL so callers can treat the buffer as a safe empty C-string.
    bool contentAddressedFirmwarePath(const char *hashHex, char *out, std::size_t outLen);
} // namespace AstrOsPathUtils
