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
} // namespace AstrOsPathUtils
