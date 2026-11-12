#pragma once

/**
 * @file sha1.h
 * @brief Fork: the SHA-1 digest, to check the hash line of the leap second list
 *
 * The leap second list (leap-seconds.list) ends with a SHA-1 of its data, which shows whether
 * the copy is whole and unchanged. That is all this is for: SHA-1 is not secure against
 * someone who changes a file on purpose.
 */

#include <array>
#include <cstdint>
#include <string>

namespace satdump
{
    /**
     * @brief The SHA-1 digest of some bytes (FIPS 180-4)
     *
     * @param data the bytes
     * @return the digest as its five 32-bit words, the first word first (the five words a
     * SHA-1 is written as in hexadecimal)
     */
    std::array<uint32_t, 5> sha1Digest(const std::string &data);
} // namespace satdump
