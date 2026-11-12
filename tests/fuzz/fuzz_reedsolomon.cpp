// Fuzz the Reed-Solomon decoders with arbitrary received codewords: RS(255,223) and
// RS(255,239), conventional and dual basis, single and interleaved (depth 4 and 5), with
// and without fill bytes. The first byte picks the setting.
#include "common/codings/reedsolomon/reedsolomon.h"
#include <cstdint>
#include <cstring>
#include <vector>
#include "fuzz_common.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 1)
        return 0;
    const uint8_t mode = data[0];
    const reedsolomon::RS_TYPE type = mode & 1 ? reedsolomon::RS239 : reedsolomon::RS223;
    const bool dual = mode & 2;
    const int depth = 1 + (mode >> 2) % 5;
    const int fill = mode & 0x80 ? 0 : -1;
    data++, size--;

    // Exactly the block the decoder may touch, so any stray access is caught
    std::vector<uint8_t> block(255 * depth);
    std::memcpy(block.data(), data, size < block.size() ? size : block.size());
    reedsolomon::ReedSolomon rs(type, fill);
    if (depth == 1)
        rs.decode(block.data(), dual);
    else
    {
        int errors[5];
        rs.decode_interlaved(block.data(), dual, depth, errors);
    }
    return 0;
}
