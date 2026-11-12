// Fuzz the CCSDS AOS packet splitter (Demuxer) with arbitrary transfer frames, in the
// layouts SatDump uses: MetOp/Meteor (882-byte zone after a 2-byte insert zone) and the
// default (884 bytes, no insert zone). Each packet is then run through the CRC and time
// readers that the instrument decoders call on it.
#include "common/ccsds/ccsds.h"
#include "common/ccsds/ccsds_aos/demuxer.h"
#include "common/ccsds/ccsds_aos/vcdu.h"
#include "common/ccsds/ccsds_time.h"
#include <cstdint>
#include <cstring>
#include <vector>
#include "fuzz_common.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 1)
        return 0;
    const bool metop_layout = data[0] & 1;
    ccsds::ccsds_aos::Demuxer demux = metop_layout ? ccsds::ccsds_aos::Demuxer(882, true) : ccsds::ccsds_aos::Demuxer(884, false);
    data++, size--;

    uint8_t cadu[1024];
    for (size_t pos = 0; pos < size; pos += 1024)
    {
        // A short last frame is zero-filled, as the file readers do
        const size_t n = size - pos < 1024 ? size - pos : 1024;
        std::memset(cadu, 0, sizeof(cadu));
        std::memcpy(cadu, data + pos, n);
        ccsds::ccsds_aos::parseVCDU(cadu);
        for (ccsds::CCSDSPacket &pkt : demux.work(cadu))
        {
            ccsds::crcCheckCCITT(pkt);
            ccsds::crcCheckVerticalParity(pkt);
            ccsds::parseCCSDSTime(pkt, 10957);
            ccsds::parseCCSDSTimeFull(pkt, 10957);
        }
    }
    return 0;
}
