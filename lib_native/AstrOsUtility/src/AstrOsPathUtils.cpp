#include "AstrOsPathUtils.hpp"

#include <string>

namespace AstrOsPathUtils
{
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
