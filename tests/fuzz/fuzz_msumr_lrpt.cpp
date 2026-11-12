// Fuzz the Meteor LRPT MSU-MR picture decoder (segments, Huffman and IDCT decoding, and
// assembling channels from segments). The input is a list of packets:
//   [apid offset 0-6] [sequence count, 2 bytes] [payload length, 2 bytes] [flags] [payload]
// flags bit 0: M2-x mode (first packet only); bit 1: the packet is cut short (a partial packet).
#include "meteor/instruments/msumr/lrpt_msumr_reader.h"
#include <cstdint>
#include "fuzz_common.h"

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
        reader.work(pkt);
    }
    for (int c = 0; c < 6; c++)
        reader.getChannel(c);
    return 0;
}
