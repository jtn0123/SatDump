// satdump::createTempFile, which replaced tmpnam() in the BitView tools: each call makes a new,
// empty file of its own in the temporary folder (TMPDIR here, set to a folder of the test's own).
#include "utils/file.h"
#include "doctest.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <unistd.h>

namespace
{
    namespace fs = std::filesystem;

    // Points TMPDIR at `dir` for as long as it lives, then puts the old value back
    struct TmpDir
    {
        std::string old;
        bool had_old;

        explicit TmpDir(const fs::path &dir)
        {
            const char *v = getenv("TMPDIR");
            had_old = v != nullptr;
            if (had_old)
                old = v;
            setenv("TMPDIR", dir.c_str(), 1);
        }

        ~TmpDir()
        {
            if (had_old)
                setenv("TMPDIR", old.c_str(), 1);
            else
                unsetenv("TMPDIR");
        }
    };
} // namespace

TEST_CASE("createTempFile: each call makes a new, empty file in the temporary folder")
{
    const fs::path dir = fs::temp_directory_path() / ("satdump-tempfile-test-" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directory(dir);
    {
        TmpDir tmp(dir);
        std::set<std::string> made;
        for (int i = 0; i < 20; i++)
        {
            std::string p = satdump::createTempFile("satdump-bitview-");
            CAPTURE(p);
            CHECK(fs::path(p).parent_path() == dir);
            CHECK(fs::path(p).filename().string().rfind("satdump-bitview-", 0) == 0);
            CHECK(fs::is_regular_file(p));
            CHECK(fs::file_size(p) == 0);
            made.insert(p);
        }
        CHECK(made.size() == 20);

        // The caller writes to it as before
        std::string p = *made.begin();
        std::ofstream(p, std::ios::binary) << "bits";
        CHECK(fs::file_size(p) == 4);
    }
    fs::remove_all(dir);
}

TEST_CASE("createTempFile: a temporary folder that does not exist is an error, not a file somewhere else")
{
    const fs::path dir = fs::temp_directory_path() / ("satdump-tempfile-missing-" + std::to_string(::getpid()));
    fs::remove_all(dir);
    TmpDir tmp(dir);
    CHECK_THROWS_AS(satdump::createTempFile(), std::exception);
    CHECK_FALSE(fs::exists(dir));
}
