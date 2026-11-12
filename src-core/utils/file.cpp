#include "file.h"
#include "core/exception.h"
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <random>
#include <sys/stat.h>
#else
#include <cstdlib>
#include <unistd.h>
#endif

namespace satdump
{
    // Return filesize
    uint64_t getFilesize(std::string filepath)
    {
        std::ifstream file(filepath, std::ios::binary | std::ios::ate);
        uint64_t fileSize = file.tellg();
        file.close();
        return fileSize;
    }

    // Fork: replaces tmpnam(), which only picked a name that another program could take
    // before the file was opened. Here the file is made in the same step as its name.
    std::string createTempFile(std::string prefix)
    {
        std::filesystem::path dir = std::filesystem::temp_directory_path();
#ifdef _WIN32
        // No mkstemp here: try random names, each created only if it does not exist yet
        std::random_device rnd;
        for (int tries = 0; tries < 100; tries++)
        {
            std::string path = (dir / (prefix + std::to_string(rnd()) + std::to_string(rnd()))).string();
            int fd = _open(path.c_str(), _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY, _S_IREAD | _S_IWRITE);
            if (fd >= 0)
            {
                _close(fd);
                return path;
            }
            if (errno != EEXIST)
                break;
        }
        throw satdump_exception("Could not create a temporary file in " + dir.string() + ": " + strerror(errno));
#else
        std::string pattern = (dir / (prefix + "XXXXXX")).string();
        std::vector<char> path(pattern.begin(), pattern.end());
        path.push_back('\0');
        int fd = mkstemp(path.data());
        if (fd < 0)
            throw satdump_exception("Could not create a temporary file in " + dir.string() + ": " + strerror(errno));
        close(fd);
        return std::string(path.data());
#endif
    }
} // namespace satdump
