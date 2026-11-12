#include "common/codings/randomization.h"
#include "doctest.h"
#include <cstdint>
#include <random>
#include <vector>

namespace
{
    // Independent CCSDS 131.0-B pseudo-randomiser: h(x) = x^8 + x^7 + x^5 + x^3 + 1, all-ones seed
    std::vector<uint8_t> reference_pn_bits(int n)
    {
        uint8_t r[8] = {1, 1, 1, 1, 1, 1, 1, 1};
        std::vector<uint8_t> out(n);
        for (int i = 0; i < n; i++)
        {
            out[i] = r[0];
            const uint8_t fb = r[0] ^ r[3] ^ r[5] ^ r[7];
            for (int k = 0; k < 7; k++)
                r[k] = r[k + 1];
            r[7] = fb;
        }
        return out;
    }
} // namespace

TEST_CASE("derand_ccsds applies the CCSDS sequence, repeating every 255 bytes")
{
    const int nbytes = 2 * 255 + 3;
    std::vector<uint8_t> data(nbytes, 0);
    derand_ccsds(data.data(), nbytes);

    const std::vector<uint8_t> bits = reference_pn_bits(nbytes * 8);
    for (int i = 0; i < nbytes; i++)
    {
        uint8_t expect = 0;
        for (int b = 0; b < 8; b++)
            expect = (expect << 1) | bits[i * 8 + b];
        CHECK_MESSAGE(data[i] == expect, "byte " << i);
    }
    CHECK(data[0] == 0xFF);
    CHECK(data[1] == 0x48);
    CHECK(data[2] == 0x0E);
    CHECK(data[3] == 0xC0);
}

TEST_CASE("derand_ccsds twice gives back the original bytes")
{
    std::mt19937 rng(1);
    std::vector<uint8_t> data(1020);
    for (auto &v : data)
        v = rng() & 0xFF;
    const std::vector<uint8_t> original = data;
    derand_ccsds(data.data(), (int)data.size());
    CHECK(data != original);
    derand_ccsds(data.data(), (int)data.size());
    CHECK(data == original);
}

TEST_CASE("soft and bit derandomisers invert exactly where the sequence has a 1")
{
    const int n = 3 * 255 + 7;
    const std::vector<uint8_t> bits = reference_pn_bits(n);

    std::vector<int8_t> soft(n, 100);
    derand_ccsds_soft(soft.data(), n);
    std::vector<uint8_t> hard(n, 0);
    derand_ccsds_bits(hard.data(), n);

    for (int i = 0; i < n; i++)
    {
        CHECK_MESSAGE(soft[i] == (bits[i] ? (int8_t)~100 : 100), "soft symbol " << i);
        CHECK_MESSAGE(hard[i] == bits[i], "bit " << i);
    }
}
