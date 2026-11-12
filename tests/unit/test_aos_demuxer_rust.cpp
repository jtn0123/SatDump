// Fork: ccsds::ccsds_aos::Demuxer in a Rust build (-DSATDUMP_WITH_RUST=ON), where it uses the
// Rust demuxer (rust/src/aos). It must give what the C++ demuxer gives, and a Rust failure
// must hand over to a new C++ demuxer as after a lost frame. Its own program
// (satdump-aos-rust-tests), as the last test switches Rust off for the rest of the run.
#include "common/ccsds/ccsds.h"
#include "common/ccsds/ccsds_aos/demuxer.h"
#include "doctest.h"
#include "log_capture.h"
#include "rust_aos.h"
#include "rust_bridge.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

namespace
{
    typedef std::vector<uint8_t> Bytes;

    struct Layout
    {
        int mpdu;
        bool has_iz;
        int iz;
        bool ext, counter, strict;
        size_t cadu() const { return size_t(12 + (has_iz ? iz : 0) + mpdu); }
    };

    // Layouts SatDump uses (MetOp, the default, GK-2A UHRIT, JPSS, Hinode), each in strict
    // mode too
    std::vector<Layout> layouts()
    {
        std::vector<Layout> out;
        for (bool strict : {false, true})
        {
            out.push_back({882, true, 2, false, true, strict});
            out.push_back({882, true, 2, false, false, strict});
            out.push_back({884, false, 2, false, false, strict});
            out.push_back({2034, false, 0, true, false, strict});
            out.push_back({1094, true, 9, false, false, strict});
            out.push_back({880, true, 0, false, false, strict});
        }
        return out;
    }

    // Packets of random lengths laid end to end and cut into CADUs of the layout, with the
    // frame counter going up by one; then `damage` random bytes changed (pointers and counters
    // included) and a few frames dropped or repeated
    std::vector<Bytes> stream(std::mt19937 &rng, const Layout &l, size_t frames, int damage)
    {
        const size_t zone = size_t(l.mpdu), head = l.cadu() - zone;
        Bytes packets;
        std::vector<size_t> starts;
        while (packets.size() < frames * zone)
        {
            const size_t len = 1 + rng() % (rng() % 4 == 0 ? 3000 : 200);
            starts.push_back(packets.size());
            const uint8_t sh = rng() % 2 ? 0x08 : 0x00;
            const uint8_t h[6] = {uint8_t(sh | (rng() & 7)), uint8_t(rng()), 0xC0, uint8_t(rng()), uint8_t((len - 1) >> 8), uint8_t(len - 1)};
            packets.insert(packets.end(), h, h + 6);
            for (size_t i = 0; i < len + (l.ext && sh ? 8 : 0); i++)
                packets.push_back(uint8_t(rng()));
        }
        std::vector<Bytes> out;
        size_t next = 0;
        for (size_t f = 0; f < frames; f++)
        {
            Bytes c(l.cadu() + 128, 0);
            c[0] = 0x1A, c[1] = 0xCF, c[2] = 0xFC, c[3] = 0x1D, c[4] = 0x40, c[5] = 9;
            c[6] = uint8_t(f >> 16), c[7] = uint8_t(f >> 8), c[8] = uint8_t(f);
            const size_t pos = f * zone;
            while (next < starts.size() && starts[next] < pos)
                next++;
            const size_t fhp = next < starts.size() && starts[next] < pos + zone ? starts[next] - pos : 2047;
            c[head - 2] = uint8_t(fhp >> 8), c[head - 1] = uint8_t(fhp);
            std::memcpy(&c[head], &packets[pos], zone);
            out.push_back(c);
        }
        for (int d = 0; d < damage; d++)
        {
            Bytes &c = out[rng() % out.size()];
            switch (rng() % 4)
            {
            case 0: // a first header pointer
                c[head - 2] = uint8_t(rng() & 7), c[head - 1] = uint8_t(rng());
                break;
            case 1: // a packet header byte or length
                c[head + rng() % zone] ^= uint8_t(1 << (rng() % 8));
                break;
            case 2: // a lost frame
                out.erase(out.begin() + rng() % out.size());
                break;
            default: // a repeated frame
            {
                const size_t k = rng() % out.size();
                const Bytes copy = out[k];
                out.insert(out.begin() + k, copy);
                break;
            }
            }
        }
        return out;
    }

    // Everything a packet carries: the header bytes and fields, and the payload
    Bytes flat(const ccsds::CCSDSPacket &p)
    {
        const ccsds::CCSDSHeader &h = p.header;
        Bytes b(h.raw, h.raw + 6);
        const uint16_t fields[] = {h.version, h.type, h.secondary_header_flag, h.apid, h.sequence_flag, h.packet_sequence_count, h.packet_length};
        for (uint16_t v : fields)
            b.push_back(uint8_t(v >> 8)), b.push_back(uint8_t(v));
        b.insert(b.end(), p.payload.begin(), p.payload.end());
        return b;
    }

    // What each CADU gives, from `first` on
    std::vector<std::vector<Bytes>> feed(ccsds::ccsds_aos::Demuxer &d, std::vector<Bytes> &cadus, size_t first = 0, size_t end = SIZE_MAX)
    {
        std::vector<std::vector<Bytes>> out;
        for (size_t i = first; i < std::min(end, cadus.size()); i++)
        {
            std::vector<Bytes> one;
            for (const ccsds::CCSDSPacket &p : d.work(cadus[i].data()))
                one.push_back(flat(p));
            out.push_back(one);
        }
        return out;
    }

    ccsds::ccsds_aos::Demuxer make(const Layout &l)
    {
        return ccsds::ccsds_aos::Demuxer(l.mpdu, l.has_iz, l.iz, l.ext, l.counter, l.strict);
    }

    // A demuxer made while Rust is paused: the C++ one, for good
    ccsds::ccsds_aos::Demuxer make_cpp(const Layout &l)
    {
        const satdump::rust::CppOnly paused;
        return make(l);
    }

    size_t count(const std::vector<std::vector<Bytes>> &per_cadu)
    {
        size_t n = 0;
        for (const auto &one : per_cadu)
            n += one.size();
        return n;
    }
} // namespace

TEST_CASE("The Demuxer gives the same packets with Rust as with C++, CADU by CADU, in every layout")
{
    REQUIRE(satdump::rust::enabled());
    std::mt19937 rng(20261112);
    for (const Layout &l : layouts())
    {
        CAPTURE(l.mpdu);
        CAPTURE(l.iz);
        CAPTURE(l.strict);
        CAPTURE(l.counter);
        REQUIRE(satdump::rust::AosDemuxer::supports(l.mpdu, l.has_iz, l.iz));
        for (int damage : {0, 3, 40})
        {
            std::vector<Bytes> cadus = stream(rng, l, 300, damage);
            ccsds::ccsds_aos::Demuxer rs = make(l), cpp = make_cpp(l);
            const auto want = feed(cpp, cadus), got = feed(rs, cadus);
            CHECK(count(want) > 100);
            CHECK(got == want);
        }
    }
    CHECK(satdump::rust::enabled());
}

TEST_CASE("A Demuxer moved in the middle of a stream goes on where it was")
{
    const Layout l{882, true, 2, false, true, false};
    std::mt19937 rng(7);
    std::vector<Bytes> cadus = stream(rng, l, 200, 0);
    ccsds::ccsds_aos::Demuxer cpp = make_cpp(l), first = make(l);
    const auto want = feed(cpp, cadus);
    auto got = feed(first, cadus, 0, 100);
    ccsds::ccsds_aos::Demuxer second(std::move(first));
    const auto rest = feed(second, cadus, 100, 150);
    got.insert(got.end(), rest.begin(), rest.end());
    ccsds::ccsds_aos::Demuxer third;
    third = std::move(second);
    const auto end = feed(third, cadus, 150);
    got.insert(got.end(), end.begin(), end.end());
    CHECK(got == want);
}

TEST_CASE("Settings the Rust code does not take, and a Demuxer made while Rust is paused, use the C++ code")
{
    CHECK_FALSE(satdump::rust::AosDemuxer::supports(5, false, 0));
    CHECK_FALSE(satdump::rust::AosDemuxer::supports(65537, false, 0));
    CHECK_FALSE(satdump::rust::AosDemuxer::supports(884, true, -1));
    CHECK_FALSE(satdump::rust::AosDemuxer::supports(884, true, 65537));
    CHECK(satdump::rust::AosDemuxer::supports(884, false, -1)); // no insert zone: its size is not used
    CHECK(satdump::rust::AosDemuxer::create(5, false, 0, false, false, false) == nullptr);
    CHECK(satdump::rust::AosDemuxer::create(884, false, 0, false, false, false) != nullptr);
    {
        const satdump::rust::CppOnly paused;
        CHECK(satdump::rust::AosDemuxer::create(884, false, 0, false, false, false) == nullptr);
    }

    // Neither reaches the Rust code, so a Rust failure set up for the next CADU does not happen
    test_log::LogCapture log;
    const Layout l{882, true, 2, false, true, false};
    ccsds::ccsds_aos::Demuxer cpp = make_cpp(l), small(5, true, 2, false, true, false);
    std::mt19937 rng(3);
    std::vector<Bytes> cadus = stream(rng, l, 20, 0);
    satdump::rust::AosDemuxer::fail_after(1);
    feed(cpp, cadus);
    feed(small, cadus);
    satdump::rust::AosDemuxer::fail_after(0);
    CHECK_FALSE(log.has(slog::LOG_ERROR, "Rust AOS demuxer failed"));
    CHECK(satdump::rust::enabled());
}

// Last: it switches Rust off for the rest of this program
TEST_CASE("A Rust failure is logged once, and a new C++ demuxer takes over from that CADU as after a lost frame")
{
    REQUIRE(satdump::rust::enabled());
    test_log::LogCapture log;
    const Layout l{882, true, 2, false, true, false};
    std::mt19937 rng(11);
    std::vector<Bytes> cadus = stream(rng, l, 120, 0);
    const size_t k = 60;  // the CADU on which the Rust code fails
    const size_t j = 90;  // where the second demuxer next works
    // A packet runs on into CADU k (its first header pointer is not 0), so one is lost there
    REQUIRE(((cadus[k][12] & 7) << 8 | cadus[k][13]) != 0);

    ccsds::ccsds_aos::Demuxer a = make(l), b = make(l);
    const auto b_before = feed(b, cadus, 0, 40);
    const auto a_before = feed(a, cadus, 0, k);
    satdump::rust::AosDemuxer::fail_after(1);
    const auto a_after = feed(a, cadus, k);
    CHECK_FALSE(satdump::rust::enabled());
    const auto b_after = feed(b, cadus, j);

    // What the C++ code gives: one demuxer on the CADUs before the failure, a new one from it
    ccsds::ccsds_aos::Demuxer whole = make(l), from_k = make(l), from_j = make(l);
    const auto all = feed(whole, cadus);
    CHECK(a_before == std::vector<std::vector<Bytes>>(all.begin(), all.begin() + k));
    CHECK(b_before == std::vector<std::vector<Bytes>>(all.begin(), all.begin() + 40));
    CHECK(a_after == feed(from_k, cadus, k));
    CHECK(b_after == feed(from_j, cadus, j));
    // A packet was in progress at CADU k, and it is lost, as after a lost frame
    CHECK(count(a_after) < count(std::vector<std::vector<Bytes>>(all.begin() + k, all.end())));

    // Logged once, though two demuxers handed over
    size_t errors = 0;
    for (const slog::LogMsg &m : log.sink->msgs)
        if (m.lvl >= slog::LOG_ERROR && m.str.find("Rust AOS demuxer failed") != std::string::npos)
            errors++;
    CHECK(errors == 1);
    // A demuxer made now is a C++ one
    CHECK(satdump::rust::AosDemuxer::create(882, true, 2, false, true, false) == nullptr);
}
