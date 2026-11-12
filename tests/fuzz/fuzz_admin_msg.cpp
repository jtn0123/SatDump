// Fuzz the MetOp admin message reader with arbitrary packet payloads. With the first byte
// odd, the rest of the input is taken as the message text and bzip2-compressed here, so the
// fuzzer reaches the XML reading directly instead of having to find valid bzip2 data.
#include "libs/bzlib/bzlib.h"
#include "metop/instruments/admin_msg/admin_msg_reader.h"
#include <cstdint>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>
#include "fuzz_common.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    // Messages that decode are written to <directory>/<sequence>.xml; keep them in one file
    static const std::string dir = [] {
        auto d = std::filesystem::temp_directory_path() / ("satdump-fuzz-admin-" + std::to_string(::getpid()));
        std::filesystem::create_directories(d);
        return d.string();
    }();
    if (size < 1)
        return 0;
    ccsds::CCSDSPacket pkt;
    pkt.header.packet_sequence_count = 1;
    if (data[0] & 1)
    {
        std::vector<char> text(data + 1, data + size);
        std::vector<char> packed(text.size() + text.size() / 100 + 700);
        unsigned int packed_size = packed.size();
        if (BZ2_bzBuffToBuffCompress(packed.data(), &packed_size, text.data(), text.size(), 1, 0, 0) != BZ_OK)
            return 0;
        pkt.payload.assign(41, 0);
        pkt.payload.insert(pkt.payload.end(), packed.begin(), packed.begin() + packed_size);
    }
    else
        pkt.payload.assign(data + 1, data + size);

    metop::admin_msg::AdminMsgReader reader;
    reader.directory = dir;
    reader.work(pkt);
    return 0;
}
