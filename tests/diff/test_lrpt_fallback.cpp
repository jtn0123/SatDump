// Fork (M6.3): what a Rust failure in the Meteor LRPT segment decoder does (rust/src/ffi.rs
// rules). A program of its own (ctest diff_lrpt_fallback), as the failure switches Rust off
// for the rest of the run. The failure is asked for with satdump::rust::lrpt_fail_next_segment,
// which makes the Rust code panic part-way through a packet, after it has decoded some blocks.
#include "doctest.h"
#include "log_capture.h"
#include "meteor/instruments/msumr/lrpt/segment.h"
#include "rust_bridge.h"
#include "rust_lrpt.h"
#include <cstring>
#include <vector>

using meteor::msumr::lrpt::Segment;

namespace
{
    // 14 blocks that each step the DC level up by one (010 1, then end of block 1010)
    std::vector<uint8_t> steps()
    {
        std::vector<uint8_t> pkt = {0x23, 0x78, 0x02, 0x93, 0x2E, 0x00, 0x01, 0xF4, 28, 0, 0, 0xFF, 0xF0, 80};
        pkt.insert(pkt.end(), 14, 0x5A);
        return pkt;
    }

    bool same(const Segment &a, const Segment &b)
    {
        return a.day_time == b.day_time && a.ms_time == b.ms_time && a.us_time == b.us_time &&
               std::memcmp(&a.timestamp, &b.timestamp, sizeof(double)) == 0 && a.MCUN == b.MCUN && a.QT == b.QT &&
               a.DC == b.DC && a.AC == b.AC && a.QFM == b.QFM && a.QF == b.QF && a.valid == b.valid &&
               a.partial == b.partial && std::memcmp(a.lines, b.lines, sizeof(a.lines)) == 0;
    }

    size_t errors(const test_log::LogCapture &log)
    {
        size_t n = 0;
        for (const slog::LogMsg &m : log.sink->msgs)
            n += m.lvl >= slog::LOG_ERROR;
        return n;
    }
} // namespace

TEST_CASE("lrpt: a Rust failure part-way through a packet gives the C++ segment, is logged once and switches Rust off")
{
    REQUIRE(satdump::rust::enabled());
    std::vector<uint8_t> pkt = steps(), copy = pkt;
    Segment want = [&] // not const: isValid() is not
    {
        const satdump::rust::CppOnly paused;
        return Segment(copy.data(), int(copy.size()), true, true);
    }();
    REQUIRE(want.isValid());

    test_log::LogCapture log;
    // Rust gets to block 6 of 14 and panics; the constructor's C++ code decodes the same packet
    satdump::rust::lrpt_fail_next_segment(6);
    const Segment got(pkt.data(), int(pkt.size()), true, true);
    CHECK(same(got, want));
    CHECK(pkt == steps()); // the packet itself is not touched
    CHECK_FALSE(satdump::rust::enabled());
    CHECK(errors(log) == 1);
    CHECK(log.has(slog::LOG_ERROR, "The Rust Meteor LRPT segment decoder failed (status 2)"));

    // From then on the C++ code decodes every packet, with nothing more logged, and the Rust
    // path says it did nothing and leaves the segment as it was
    for (int k = 0; k < 3; k++)
    {
        const Segment again(pkt.data(), int(pkt.size()), true, true);
        CHECK(same(again, want));
    }
    Segment probe;
    probe.MCUN = 77;
    CHECK_FALSE(probe.decodeRust(pkt.data(), int(pkt.size()), true, true));
    CHECK(probe.MCUN == 77);
    CHECK_FALSE(probe.valid);
    CHECK(errors(log) == 1);
}
