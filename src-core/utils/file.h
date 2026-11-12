#pragma once

/**
 * @file file.h
 * @brief File manipulation utilities
 */

#include <cstdint>
#include <string>

namespace satdump
{
    /**
     * @brief Gets the filesize in bytes
     *
     * @param filepath path of the file to check
     * @return size in bytes
     */
    uint64_t getFilesize(std::string filepath);

    /**
     * @brief Creates a new, empty file with a unique name in the
     * system's temporary folder (TMPDIR, or /tmp) and returns its path.
     * The file is created only if no file of that name exists, so no other
     * program can have made or replaced it first (as could happen with tmpnam).
     * Throws if the file cannot be created.
     *
     * @param prefix start of the file name
     * @return path of the new file
     */
    std::string createTempFile(std::string prefix = "satdump-");
} // namespace satdump