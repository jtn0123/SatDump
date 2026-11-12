// Fork (M6.3): the Meteor LRPT segment decoder in Rust as SatDump uses it: the Segment
// constructor goes through Rust when it is on and gives what the C++ gives, a bad argument
// leaves the work to the C++ without switching Rust off, and the corpus reader of the
// lrpt_segment twin finds the MSU-MR packets in CADUs. Every packet shape is compared by the
// twin itself (ctest diff_twins); a Rust failure is tested in test_lrpt_fallback.cpp.
#include "diff.h"
#include "doctest.h"
#include "meteor/instruments/msumr/lrpt/segment.h"
#include "rust_bridge.h"
#include "rust_lrpt.h"
#include "test_diff.h"
#include <cstring>
#include <fstream>

using namespace satdump::diff;
using meteor::msumr::lrpt::Segment;
namespace fs = std::filesystem;

namespace
{
    bool same(const Segment &a, const Segment &b)
    {
        return a.day_time == b.day_time && a.ms_time == b.ms_time && a.us_time == b.us_time &&
               std::memcmp(&a.timestamp, &b.timestamp, sizeof(double)) == 0 && a.MCUN == b.MCUN && a.QT == b.QT &&
               a.DC == b.DC && a.AC == b.AC && a.QFM == b.QFM && a.QF == b.QF && a.valid == b.valid &&
               a.partial == b.partial && std::memcmp(a.lines, b.lines, sizeof(a.lines)) == 0;
    }

    // The 14 blocks of a packet each "DC difference +1, end of block" (010 1 1010): 0x5A
    std::vector<uint8_t> steps()
    {
        std::vector<uint8_t> pkt = {0x23, 0x78, 0x02, 0x93, 0x2E, 0x00, 0x01, 0xF4, 28, 0, 0, 0xFF, 0xF0, 80};
        pkt.insert(pkt.end(), 14, 0x5A);
        return pkt;
    }

    // A CADU of virtual channel 5 whose packet zone (882 bytes) is `zone`
    std::vector<uint8_t> cadu(uint8_t vcid, uint32_t counter, uint16_t first_header, const std::vector<uint8_t> &zone)
    {
        std::vector<uint8_t> c = {0x1A, 0xCF, 0xFC, 0x1D, 0x40, vcid, uint8_t(counter >> 16), uint8_t(counter >> 8),
                                  uint8_t(counter), 0, 0, 0, uint8_t(first_header >> 8), uint8_t(first_header)};
        c.insert(c.end(), zone.begin(), zone.end());
        c.resize(1024, 0);
        return c;
    }

    std::vector<uint8_t> ccsds_packet(uint16_t apid, const std::vector<uint8_t> &payload)
    {
        const size_t n = payload.size() - 1;
        std::vector<uint8_t> p = {uint8_t(0x08 | apid >> 8), uint8_t(apid), 0xC0, 0, uint8_t(n >> 8), uint8_t(n)};
        p.insert(p.end(), payload.begin(), payload.end());
        return p;
    }
} // namespace

TEST_CASE("lrpt: the Segment constructor uses Rust and gives what the C++ gives")
{
    REQUIRE(satdump::rust::enabled());
    Rng r(20261112);
    for (int k = 0; k < 200; k++)
    {
        // The packet of 0x5A blocks, cut and damaged in different ways, then any bytes
        std::vector<uint8_t> pkt = steps();
        if (k < 100)
        {
            pkt.resize(size_t(k) % (pkt.size() + 1));
            if (k >= 50 && !pkt.empty())
                pkt[size_t(r.below(pkt.size()))] ^= uint8_t(1 << r.below(8));
        }
        else
            pkt = fill(20261112, uint64_t(k), size_t(r.below(300)), Style::Random);
        const bool partial = k % 2, m2x = k % 3 == 0;

        Segment probe;
        CHECK(probe.decodeRust(pkt.data(), int(pkt.size()), partial, m2x));
        const Segment rs(pkt.data(), int(pkt.size()), partial, m2x);
        std::vector<uint8_t> copy = pkt;
        satdump::rust::CppOnly paused;
        const Segment cpp(copy.data(), int(copy.size()), partial, m2x);
        CHECK_MESSAGE(same(rs, cpp), "packet " << k);
        CHECK_MESSAGE(same(probe, cpp), "packet " << k);
    }
    CHECK(satdump::rust::enabled());

    // The good packet: valid, its time stamp as parseCCSDSTimeFullRaw gives it, and a DC level
    // that steps up block by block
    std::vector<uint8_t> pkt = steps();
    Segment s(pkt.data(), int(pkt.size()), false, false); // isValid() is not const
    CHECK(s.isValid());
    CHECK(s.timestamp == 9080.0 * 86400.0 + 43200.0 + 0.0005);
    CHECK(s.lines[0][0] < s.lines[7][111]);
}

TEST_CASE("lrpt: a negative length is refused by Rust, the C++ decodes it and Rust stays on")
{
    REQUIRE(satdump::rust::enabled());
    std::vector<uint8_t> pkt = steps();
    Segment probe;
    probe.MCUN = 77;
    CHECK_FALSE(probe.decodeRust(pkt.data(), -1, false, true));
    CHECK(probe.MCUN == 77); // unchanged
    const Segment s(pkt.data(), -1, false, true);
    CHECK_FALSE(s.valid);
    CHECK(s.timestamp == -1.0);
    CHECK(satdump::rust::enabled());
    satdump::rust::LrptSegment out;
    CHECK_FALSE(satdump::rust::lrpt_segment(pkt.data(), int(pkt.size()), false, false, nullptr));
    CHECK_FALSE(satdump::rust::lrpt_segment(nullptr, 5, false, false, &out));
    int64_t table[64];
    CHECK_FALSE(satdump::rust::lrpt_quant_table(256, table));
    CHECK(satdump::rust::enabled());
}

TEST_CASE("lrpt: the twin reads the MSU-MR packets out of CADUs, and other files as the fuzz harness does")
{
    TempOptions t;
    const Twin *twin = find_twin("lrpt_segment");
    REQUIRE(twin != nullptr);
    const fs::path folder = fs::path(t.opt.failures) / "corpus";
    fs::create_directories(folder);

    // Two packets of virtual channel 5 (one MSU-MR, one telemetry), the rest of the zone an
    // idle packet; a frame of another virtual channel; then the next frame of channel 5
    const std::vector<uint8_t> segment = steps();
    std::vector<uint8_t> zone = ccsds_packet(64, segment);
    const std::vector<uint8_t> tlm = ccsds_packet(70, std::vector<uint8_t>(30, 7));
    zone.insert(zone.end(), tlm.begin(), tlm.end());
    zone = [&]
    {
        std::vector<uint8_t> z = zone;
        const std::vector<uint8_t> idle = ccsds_packet(2047, std::vector<uint8_t>(882 - z.size() - 6, 0));
        z.insert(z.end(), idle.begin(), idle.end());
        return z;
    }();
    std::vector<uint8_t> file = cadu(5, 0, 0, zone);
    const std::vector<uint8_t> other = cadu(9, 0, 0, std::vector<uint8_t>(882, 0x55));
    file.insert(file.end(), other.begin(), other.end());
    const std::vector<uint8_t> next = cadu(5, 1, 0, ccsds_packet(2047, std::vector<uint8_t>(876, 0)));
    file.insert(file.end(), next.begin(), next.end());
    std::ofstream(folder / "pass.cadu", std::ios::binary).write((const char *)file.data(), std::streamsize(file.size()));

    const std::vector<Case> cases = corpus_cases(*twin, (folder / "pass.cadu").string());
    REQUIRE(cases.size() == 2); // the packet, and a copy with one bit flipped
    CHECK(cases[0].input == segment);
    CHECK(cases[0].param("partial", -1) == 0);
    CHECK(cases[0].param("m2x", -1) == 1);
    CHECK(cases[0].inline_input);
    CHECK(cases[1].input != segment);
    CHECK(cases[1].input.size() == segment.size());
    CHECK(run_cases(*twin, cases, t.opt).differences == 0);

    // Virtual channel 5 with no MSU-MR packet: the corpus would check nothing
    std::vector<uint8_t> empty = cadu(5, 0, 0, ccsds_packet(2047, std::vector<uint8_t>(876, 0)));
    std::ofstream(folder / "empty.cadu", std::ios::binary).write((const char *)empty.data(), std::streamsize(empty.size()));
    CHECK_THROWS_WITH(corpus_cases(*twin, (folder / "empty.cadu").string()), doctest::Contains("no MSU-MR packets"));

    // A fuzz input: [APID offset] [sequence, 2] [length, 2] [flags] [payload]; flags bit 1 cut short
    const std::vector<uint8_t> fuzz = {0, 0, 0, 0, 3, 2, 0xA, 0xB, 0xC, 1, 0, 0, 0, 2, 1, 0xD, 0xE};
    std::ofstream(folder / "fuzz.bin", std::ios::binary).write((const char *)fuzz.data(), std::streamsize(fuzz.size()));
    const std::vector<Case> f = corpus_cases(*twin, (folder / "fuzz.bin").string());
    REQUIRE(f.size() == 4);
    CHECK(f[0].input == std::vector<uint8_t>{0xA, 0xB, 0xC});
    CHECK(f[0].param("partial", -1) == 1);
    CHECK(f[2].input == std::vector<uint8_t>{0xD, 0xE});
    CHECK(f[2].param("partial", -1) == 0);
    CHECK(run_cases(*twin, f, t.opt).differences == 0);
}
