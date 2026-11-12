// Fork: the Rust parts must give exactly what the C++ code gives, and a Rust failure must come
// back as a status instead of stopping SatDump. Built only with -DSATDUMP_WITH_RUST=ON.
#include "common/codings/randomization.h"
#include "doctest.h"
#include "rust_bridge.h"
#include <random>
#include <vector>

TEST_CASE("Rust derandomiser gives the same bytes as the C++ one for every length 0-4096")
{
    REQUIRE(satdump::rust::enabled());
    std::mt19937 rng(20261111);
    std::vector<uint8_t> input(4096 + 8);
    int not_done = 0, mismatches = 0;
    for (int length = 0; length <= 4096; length++)
    {
        const int start = length % 8; // different alignments too
        for (auto &v : input)
            v = rng() & 0xFF;
        std::vector<uint8_t> cpp = input, rs = input;
        derand_ccsds_cpp(cpp.data() + start, length);
        if (!satdump::rust::derand_ccsds(rs.data() + start, length))
            not_done++;
        if (cpp != rs)
            mismatches++;
    }
    CHECK(not_done == 0);
    CHECK(mismatches == 0);
}

TEST_CASE("derand_ccsds (which uses Rust in this build) matches the C++ one on a CADU")
{
    std::mt19937 rng(7);
    std::vector<uint8_t> cadu(1024);
    for (auto &v : cadu)
        v = rng() & 0xFF;
    std::vector<uint8_t> cpp = cadu;
    derand_ccsds(&cadu[4], 1020);
    derand_ccsds_cpp(&cpp[4], 1020);
    CHECK(cadu == cpp);
}

TEST_CASE("A Rust panic comes back as a status and SatDump carries on")
{
    CHECK(satdump::rust::panic_check() == satdump::rust::STATUS_PANIC);
    CHECK(satdump::rust::enabled()); // the deliberate panic does not switch Rust off
}

TEST_CASE("Bad arguments are refused and change nothing")
{
    std::vector<uint8_t> data = {1, 2, 3};
    CHECK_FALSE(satdump::rust::derand_ccsds(nullptr, 5));
    CHECK_FALSE(satdump::rust::derand_ccsds(data.data(), -1));
    CHECK(data == std::vector<uint8_t>{1, 2, 3});
    CHECK(satdump::rust::derand_ccsds(nullptr, 0));
    CHECK(satdump::rust::enabled()); // a bad argument is not a Rust failure
}
