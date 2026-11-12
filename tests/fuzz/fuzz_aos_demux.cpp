// Fuzz the CCSDS AOS packet splitter (Demuxer) with arbitrary transfer frames, in the
// layouts SatDump uses: MetOp/Meteor (882-byte zone after a 2-byte insert zone, with the
// frame counter check MetOp turns on, or without it as for Meteor) and the default (884
// bytes, no insert zone).
// Every packet must be whole (as long as its header says); each is then run through the
// CRC and time readers that the instrument decoders call on it.
#include "common/ccsds/ccsds.h"
#include "common/ccsds/ccsds_aos/demuxer.h"
#include "common/ccsds/ccsds_aos/vcdu.h"
#include "common/ccsds/ccsds_time.h"
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "fuzz_common.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 1)
        return 0;
    const bool metop_layout = data[0] & 1;
    // Frames follow on from each other (frame counter + 1), so packets spanning frames are
    // reached; bit 1 of the first byte keeps the input's own counters, to fuzz lost frames too
    const bool own_counters = data[0] & 2;
    // Bit 2 picks the 882-byte layout without the frame counter check (the Meteor decoder)
    const bool check_counter = !(data[0] & 4);
    ccsds::ccsds_aos::Demuxer demux = metop_layout ? ccsds::ccsds_aos::Demuxer(882, true, 2, false, check_counter) : ccsds::ccsds_aos::Demuxer(884, false);
    data++, size--;

    uint8_t cadu[1024];
    for (size_t pos = 0; pos < size; pos += 1024)
    {
        // A short last frame is zero-filled, as the file readers do
        const size_t n = size - pos < 1024 ? size - pos : 1024;
        std::memset(cadu, 0, sizeof(cadu));
        std::memcpy(cadu, data + pos, n);
        if (!own_counters)
        {
            const uint32_t count = uint32_t(pos / 1024);
            cadu[6] = uint8_t(count >> 16), cadu[7] = uint8_t(count >> 8), cadu[8] = uint8_t(count);
        }
        ccsds::ccsds_aos::parseVCDU(cadu);
        for (ccsds::CCSDSPacket &pkt : demux.work(cadu))
        {
            if (pkt.payload.size() != size_t(pkt.header.packet_length) + 1)
                std::abort(); // a cut-off or overlong packet: the demuxer must drop it
            ccsds::crcCheckCCITT(pkt);
            ccsds::crcCheckVerticalParity(pkt);
            ccsds::parseCCSDSTime(pkt, 10957);
            ccsds::parseCCSDSTimeFull(pkt, 10957);
        }
    }
    return 0;
}
