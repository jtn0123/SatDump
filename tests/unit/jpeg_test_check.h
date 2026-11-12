// Fork: the pinned-hash check of the JPEG tests (test_jpeg8_decompress.cpp, test_jpeg12.cpp)
#pragma once

#include "doctest.h"
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace jpeg_test
{
    struct Expected
    {
        const char *name;
        uint64_t hash;
    };

    // Checks a hash against the test's table. A name missing from the table fails, and the
    // table line for it is printed, so that new entries can be recorded.
    template <size_t N>
    void check(const Expected (&table)[N], const std::string &name, uint64_t h)
    {
        bool found = false;
        uint64_t want = 0;
        for (auto &e : table)
            if (name == e.name)
            {
                found = true;
                want = e.hash;
            }
        INFO(name);
        if (!found)
        {
            char line[300];
            snprintf(line, sizeof(line), "{\"%s\", 0x%016llxULL},", name.c_str(), (unsigned long long)h);
            MESSAGE(line);
        }
        CHECK(found);
        CHECK(h == want);
    }
} // namespace jpeg_test
