#pragma once

/**
 * @file file_url.h
 * @brief Fork: local files named by file:// URLs (orbit elements and Earth orientation data)
 *
 * The settings that name where SatDump gets its orbit elements and IERS data from can name a
 * local file instead of a web address, as file:///path/to/file. Such a file is read straight
 * from disk: there is no download, and a file that is missing is reported once instead of
 * being tried again ten times.
 */

#include <string>

namespace satdump
{
    /**
     * @brief Whether a URL names a local file (it starts with file://)
     */
    bool isFileUrl(const std::string &url);

    /**
     * @brief Read the file a file:// URL names
     *
     * Takes file:///path and file://localhost/path. Nothing is percent-decoded, so the path
     * is used as written.
     *
     * @param url the file:// URL
     * @param content the whole file, when it could be read
     * @param error why the file could not be read, when it could not
     * @return true if the file was read
     */
    bool readFileUrl(const std::string &url, std::string &content, std::string &error);
} // namespace satdump
