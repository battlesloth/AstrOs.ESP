#include "AstrOsPathUtils.hpp"

#include <cstdio>
#include <string>

namespace AstrOsPathUtils
{
    bool contentAddressedFirmwarePath(const char *hashHex, char *out, std::size_t outLen)
    {
        if (out != nullptr && outLen > 0)
        {
            out[0] = '\0';
        }
        if (out == nullptr || outLen < FIRMWARE_PATH_BUF_LEN || hashHex == nullptr)
        {
            return false;
        }
        // Need at least FIRMWARE_HASH_PREFIX_LEN chars before the NUL — a
        // shorter input would let snprintf's precision-clipped "%.*s" silently
        // truncate to whatever it found, producing names that don't reflect
        // the digest.
        for (std::size_t i = 0; i < FIRMWARE_HASH_PREFIX_LEN; ++i)
        {
            if (hashHex[i] == '\0')
            {
                return false;
            }
        }
        int written =
            std::snprintf(out, outLen, "%s%.*s.bin", FIRMWARE_DIR, static_cast<int>(FIRMWARE_HASH_PREFIX_LEN), hashHex);
        if (written <= 0 || static_cast<std::size_t>(written) >= outLen)
        {
            out[0] = '\0';
            return false;
        }
        return true;
    }

    bool isPathSafe(const std::string &path, std::string &reasonOut)
    {
        reasonOut.clear();

        if (path.empty())
        {
            reasonOut = "empty path rejected";
            return false;
        }
        if (path[0] == '/')
        {
            reasonOut = "absolute path rejected: " + path;
            return false;
        }
        // Conservative: rejects any path containing ".." as a substring, not
        // just at component boundaries. False positives are benign given
        // current naming conventions.
        if (path.find("..") != std::string::npos)
        {
            reasonOut = "traversal component rejected: " + path;
            return false;
        }
        if (path.find("//") != std::string::npos)
        {
            reasonOut = "double-slash rejected: " + path;
            return false;
        }
        if (path.size() > MAX_PATH_LEN)
        {
            std::string truncated = path.substr(0, 40);
            reasonOut = "path too long (" + std::to_string(path.size()) + " > " + std::to_string(MAX_PATH_LEN) +
                        "): " + truncated + "...";
            return false;
        }
        return true;
    }
} // namespace AstrOsPathUtils
