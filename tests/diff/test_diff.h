#pragma once

// Fork: what the differential-test framework's own tests share (test_diff*.cpp)
#include "diff.h"
#include <filesystem>
#include <string>
#include <system_error>
#include <unistd.h>

// Reproducers from these tests go to a folder of their own, removed afterwards
struct TempOptions
{
    satdump::diff::Options opt;
    TempOptions()
    {
        opt.failures = (std::filesystem::temp_directory_path() / ("satdump-diff-test-" + std::to_string(getpid()))).string();
        opt.max_printed = 2;
    }
    ~TempOptions()
    {
        std::error_code ec;
        std::filesystem::remove_all(opt.failures, ec);
    }
};
