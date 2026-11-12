// Fork: see file_url.h
#include "file_url.h"
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace satdump
{
    bool isFileUrl(const std::string &url) { return url.rfind("file://", 0) == 0; }

    bool readFileUrl(const std::string &url, std::string &content, std::string &error)
    {
        if (!isFileUrl(url))
        {
            error = "not a file:// URL";
            return false;
        }

        std::string path = url.substr(7);
        if (path.rfind("localhost/", 0) == 0)
            path = path.substr(9);
        if (path.empty() || path[0] != '/')
        {
            error = "a file URL needs a full path, as file:///folder/file";
            return false;
        }

        std::error_code ec;
        if (!std::filesystem::exists(path, ec))
        {
            error = "no such file";
            return false;
        }
        if (std::filesystem::is_directory(path, ec))
        {
            error = "it is a folder";
            return false;
        }

        errno = 0;
        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            error = std::string("cannot be opened: ") + (errno != 0 ? std::strerror(errno) : "unknown error");
            return false;
        }

        std::ostringstream text;
        text << file.rdbuf();
        if (file.bad())
        {
            error = "read error";
            return false;
        }

        content = text.str();
        return true;
    }
} // namespace satdump
