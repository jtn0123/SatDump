#include "common/codings/reedsolomon/reedsolomon.h"
#include "doctest.h"
#include <algorithm>
#include <cstdint>
#include <numeric>
#include <random>
#include <vector>

using reedsolomon::ReedSolomon;

namespace
{
    // A buffer of exactly n bytes with guard bytes on both sides, to catch writes past either end
    struct Guarded
    {
        static constexpr size_t PAD = 64;
        static constexpr uint8_t GUARD = 0xA5;
        std::vector<uint8_t> mem;
        size_t n;
        explicit Guarded(size_t n) : mem(n + 2 * PAD, GUARD), n(n) {}
        uint8_t *data() { return mem.data() + PAD; }
        bool intact() const
        {
            for (size_t i = 0; i < PAD; i++)
                if (mem[i] != GUARD || mem[PAD + n + i] != GUARD)
                    return false;
            return true;
        }
    };

    // Flip `count` different bytes among the first `span` bytes of one codeword. decode()
    // counts corrected data bytes only, so tests that check the count corrupt data bytes.
    // (Positions are codeword indices; `stride`/`offset` map them into an interleaved block.)
    void corrupt(uint8_t *data, int count, std::mt19937 &rng, int span = 255, int stride = 1, int offset = 0)
    {
        std::vector<int> pos(span);
        std::iota(pos.begin(), pos.end(), 0);
        std::shuffle(pos.begin(), pos.end(), rng);
        for (int k = 0; k < count; k++)
            data[pos[k] * stride + offset] ^= 1 + (rng() % 255);
    }

    void fill_random(uint8_t *data, size_t n, std::mt19937 &rng)
    {
        for (size_t i = 0; i < n; i++)
            data[i] = rng() & 0xFF;
    }
} // namespace

TEST_CASE("RS(255,223) and RS(255,239) correct up to their limit, with and without dual basis")
{
    struct Case
    {
        reedsolomon::RS_TYPE type;
        int k, t;
    };
    for (const Case c : {Case{reedsolomon::RS223, 223, 16}, Case{reedsolomon::RS239, 239, 8}})
        for (const bool dual : {false, true})
        {
            CAPTURE(c.k);
            CAPTURE(dual);
            std::mt19937 rng(c.k + dual);
            ReedSolomon rs(c.type);
            Guarded block(255);
            fill_random(block.data(), c.k, rng);
            rs.encode(block.data(), dual);
            const std::vector<uint8_t> sent(block.data(), block.data() + 255);

            corrupt(block.data(), c.t, rng, c.k);
            CHECK(rs.decode(block.data(), dual) == c.t);
            CHECK(std::equal(sent.begin(), sent.begin() + c.k, block.data()));
            CHECK(block.intact());
        }
}

TEST_CASE("RS(255,223) reports an uncorrectable codeword as -1 and leaves it as received")
{
    std::mt19937 rng(7);
    ReedSolomon rs(reedsolomon::RS223);
    Guarded block(255);
    fill_random(block.data(), 223, rng);
    rs.encode(block.data(), true);
    corrupt(block.data(), 60, rng);
    const std::vector<uint8_t> received(block.data(), block.data() + 255);

    CHECK(rs.decode(block.data(), true) == -1);
    CHECK(std::equal(received.begin(), received.end(), block.data()));
    CHECK(block.intact());
}

TEST_CASE("Interleaved RS (depth 4, a 1020-byte CADU body) stays inside its block for every fill setting")
{
    // fill -1 is the "no fill" default used by the Meteor LRPT 72k pipeline
    for (const int fill : {-1, 0})
    {
        CAPTURE(fill);
        std::mt19937 rng(11);
        ReedSolomon rs(reedsolomon::RS223, fill);
        Guarded block(255 * 4);
        fill_random(block.data(), 255 * 4, rng);
        rs.encode_interlaved(block.data(), true, 4);
        CHECK(block.intact());
        const std::vector<uint8_t> sent(block.data(), block.data() + 255 * 4);

        for (int b = 0; b < 4; b++)
            corrupt(block.data(), 16, rng, 223, 4, b);

        int errors[4] = {-2, -2, -2, -2};
        rs.decode_interlaved(block.data(), true, 4, errors);
        for (int b = 0; b < 4; b++)
            CHECK(errors[b] == 16);
        // Data bytes (the first 223 of each codeword) are restored
        CHECK(std::equal(sent.begin(), sent.begin() + 223 * 4, block.data()));
        CHECK(block.intact());
    }
}
