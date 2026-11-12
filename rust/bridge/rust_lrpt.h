#pragma once

// Fork (M6.3): the Meteor LRPT MSU-MR segment decoder in Rust (rust/src/lrpt), for the
// plugin's Segment class (plugins/meteor_support/meteor/instruments/msumr/lrpt/segment.cpp).
// Built only with -DSATDUMP_WITH_RUST=ON. The rules of rust_bridge.h apply: false means Rust
// did not do the work and the output is unchanged; the C++ decoder then decodes the packet.

#include <cstdint>

namespace satdump::rust
{
    /// One decoded segment: the fields of meteor::msumr::lrpt::Segment. The layout is that of
    /// LrptSegment in rust/src/ffi_lrpt.rs (rust_lrpt.cpp checks it); valid and partial are 0 or 1.
    struct LrptSegment
    {
        double timestamp;
        uint32_t ms_time;
        uint16_t day_time;
        uint16_t us_time;
        uint16_t qfm;
        uint8_t mcun;
        uint8_t qt;
        uint8_t dc;
        uint8_t ac;
        uint8_t qf;
        uint8_t valid;
        uint8_t partial;
        uint8_t lines[8][14 * 8];
    };

    /// Decodes one MSU-MR packet payload as the C++ Segment(data, length, partial, m2x) does
    bool lrpt_segment(const uint8_t *data, int length, bool partial, bool m2x, LrptSegment *out);

    /// The quantisation table for quality factor `qf` (0 to 255), the twin of
    /// GetQuantizationTable((float)qf). Not used by SatDump: the differential tests (tests/diff)
    /// compare it with the C++ one for every quality factor.
    bool lrpt_quant_table(int qf, int64_t table[64]);

    /// For tests: the next lrpt_segment call on this thread makes the Rust code panic when it
    /// reaches block `block` (0 to 13) of its packet, so a test can show what a failure does
    void lrpt_fail_next_segment(int block);
} // namespace satdump::rust
