// The ELEKTRO / Arktika GGAK frame merger (elektro_arktika::ggak::GGAKMerger), which puts the
// 224-byte GGAK frames from one or more receivers back in order by their master counter.
// The frames are built by hand: an id that is not 119 (filler), the counter, a payload that
// depends on the counter, and the 16-bit byte sum the CRC check expects.
#include "elektro_arktika/ggak/ggak.h"
#include "elektro_arktika/ggak/merger.h"
#include "doctest.h"
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// Counter lists print as runs, e.g. "0-9 11 10 12-29 31-99"
namespace doctest
{
    template <>
    struct StringMaker<std::vector<int>>
    {
        static String convert(const std::vector<int> &v)
        {
            std::string s;
            for (size_t i = 0; i < v.size();)
            {
                size_t j = i;
                while (j + 1 < v.size() && v[j + 1] == v[j] + 1)
                    j++;
                s += (s.empty() ? "" : " ") + std::to_string(v[i]) + (j > i ? "-" + std::to_string(v[j]) : "");
                i = j + 1;
            }
            return s.c_str();
        }
    };
} // namespace doctest

namespace
{
    using elektro_arktika::ggak::GGAKFrame;
    using elektro_arktika::ggak::GGAKMerger;

    GGAKFrame frame(uint16_t counter, uint8_t id = 1)
    {
        uint8_t raw[224] = {0};
        raw[0] = 0x1A;
        raw[1] = 0xCF;
        raw[2] = 0xFC;
        raw[3] = 0x1D;
        raw[4] = id; // 119 is a filler frame
        raw[5] = uint8_t(counter >> 8);
        raw[6] = uint8_t(counter & 0xFF);
        for (int i = 13; i < 222; i++)
            raw[i] = uint8_t(counter * 13 + i);
        uint16_t sum = 0;
        for (int i = 0; i < 222; i++)
            sum += raw[i];
        raw[222] = uint8_t(sum >> 8);
        raw[223] = uint8_t(sum & 0xFF);

        GGAKFrame f;
        std::memcpy(&f, raw, sizeof(f));
        REQUIRE(elektro_arktika::ggak::checkCRC(&f));
        return f;
    }

    // Feeds the frames with these counters, in this order, and returns the counters of the
    // frames that come out
    std::vector<int> merge(const std::vector<int> &counters)
    {
        GGAKMerger merger;
        std::vector<int> out;
        for (int c : counters)
            for (auto &f : merger.process(frame(uint16_t(c))))
                out.push_back(int(uint16_t(f.master_counter)));
        return out;
    }

    std::vector<int> range(int from, int to)
    {
        std::vector<int> v;
        for (int i = from; i < to; i++)
            v.push_back(i);
        return v;
    }

    // 0 .. n-1 with each pair (a, a + 1) for a in `swaps` received the other way round
    std::vector<int> swapped(int n, const std::vector<int> &swaps)
    {
        std::vector<int> v = range(0, n);
        for (int a : swaps)
            std::swap(v[a], v[a + 1]);
        return v;
    }
} // namespace

TEST_CASE("GGAK merger: frames in order all come out, in order")
{
    CHECK(merge(range(0, 100)) == range(0, 100));
}

TEST_CASE("GGAK merger: two frames received the wrong way round come out in order, and no later frame is lost")
{
    // Before the fix the recovered frames stayed in the recovery queue, so the merger never went
    // back to normal: about 20 frames later the queue overflowed and a good frame was dropped.
    CHECK(merge(swapped(100, {10})) == range(0, 100));
    CHECK(merge(swapped(300, {10, 60, 121, 250})) == range(0, 300));
}

TEST_CASE("GGAK merger: after a recovery, duplicates are skipped again")
{
    std::vector<int> in = swapped(60, {10});
    in.insert(in.begin() + 40, 38); // a duplicate of a frame that already came out
    in.insert(in.begin() + 45, 43);
    CHECK(merge(in) == range(0, 60));
}

TEST_CASE("GGAK merger: frames with a bad checksum and filler frames are skipped")
{
    GGAKMerger merger;
    GGAKFrame bad = frame(5);
    bad.payload[0] ^= 1;
    CHECK(merger.process(bad).empty());

    CHECK(merger.process(frame(5, 119)).empty());

    auto out = merger.process(frame(5));
    REQUIRE(out.size() == 1);
    CHECK(int(uint16_t(out[0].master_counter)) == 5);
}
