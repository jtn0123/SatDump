// Fuzz the Meteor LRPT MSU-MR picture decoder (segments, Huffman and IDCT decoding, and
// assembling channels from segments). The input is a list of packets:
//   [apid offset 0-6] [sequence count, 2 bytes] [payload length, 2 bytes] [flags] [payload]
// flags bit 0: M2-x mode (first packet only); bit 1: the packet is cut short (a partial packet).
// Fork (M6.3): in a Rust build the reader's segments are decoded in Rust, and each packet is
// also decoded by both the C++ and the Rust decoder, which must give the same segment.
#include "meteor/instruments/msumr/lrpt_msumr_reader.h"
#include <cstdint>
#include "fuzz_common.h"
#ifdef SATDUMP_WITH_RUST
#include "meteor/instruments/msumr/lrpt/segment.h"
#include "rust_bridge.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
    void compare_segments(ccsds::CCSDSPacket &pkt, bool m2x)
    {
        using meteor::msumr::lrpt::Segment;
        if (!satdump::rust::enabled()) // SATDUMP_NO_RUST=1: nothing to compare
            return;
        const int length = (int)pkt.payload.size();
        const bool partial = pkt.payload.size() - 1 != pkt.header.packet_length; // as the reader works it out
        std::vector<uint8_t> copy = pkt.payload;
        Segment rs;
        if (!rs.decodeRust(pkt.payload.data(), length, partial, m2x))
        {
            std::fprintf(stderr, "fuzz_msumr_lrpt: the Rust segment decoder failed\n");
            std::abort();
        }
        const satdump::rust::CppOnly paused;
        const Segment cpp(copy.data(), length, partial, m2x);
        const bool same = rs.day_time == cpp.day_time && rs.ms_time == cpp.ms_time && rs.us_time == cpp.us_time &&
                          std::memcmp(&rs.timestamp, &cpp.timestamp, sizeof(double)) == 0 && rs.MCUN == cpp.MCUN &&
                          rs.QT == cpp.QT && rs.DC == cpp.DC && rs.AC == cpp.AC && rs.QFM == cpp.QFM && rs.QF == cpp.QF &&
                          rs.valid == cpp.valid && rs.partial == cpp.partial &&
                          std::memcmp(rs.lines, cpp.lines, sizeof(rs.lines)) == 0;
        if (!same)
        {
            std::fprintf(stderr, "fuzz_msumr_lrpt: the C++ and Rust segment decoders differ on a %d-byte packet\n", length);
            std::abort();
        }
    }
} // namespace
#endif

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 6)
        return 0;
    meteor::msumr::lrpt::MSUMRReader reader(data[5] & 1);
    size_t pos = 0;
    while (pos + 6 <= size)
    {
        ccsds::CCSDSPacket pkt;
        pkt.header.apid = 64 + data[pos] % 7;
        pkt.header.packet_sequence_count = (data[pos + 1] << 8 | data[pos + 2]) & 0x3FFF;
        const size_t want = (data[pos + 3] << 8 | data[pos + 4]) % 2048;
        const bool partial = data[pos + 5] & 2;
        pos += 6;
        const size_t len = want < size - pos ? want : size - pos;
        pkt.payload.assign(data + pos, data + pos + len);
        pkt.header.packet_length = partial ? len + 7 : (len ? len - 1 : 0);
        pos += len;
#ifdef SATDUMP_WITH_RUST
        compare_segments(pkt, data[5] & 1);
#endif
        reader.work(pkt);
    }
    for (int c = 0; c < 6; c++)
        reader.getChannel(c);
    return 0;
}
