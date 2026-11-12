// Fuzz SatDump's J2K (JPEG 2000 codestream) decoding, src-core/image/j2k_stream.h: the memory
// stream, the decode with its size limit and the conversion of a decoded picture, as used for
// GOES GRB, FY-4, GK-2A and ORB data held in memory (decompress_j2k_openjp2) and for .j2k files
// (load_j2k). The first byte picks how the rest of the input is used:
//   bit 0 clear  the rest is the codestream
//   bit 0 set    one of a few good codestreams saved here (picked by the other bits) is cut to
//                the length in the next two bytes, and the bytes after the next two (which give
//                a position) are xor-ed over it from there, so the fuzzer gets past the header
//                quickly and into the tile data
// OpenJPEG is the system library and is not instrumented, so the fuzzer sees coverage of
// SatDump's code only; the good codestreams and j2k.dict make up for some of that.
#include "image/image.h"
#include "image/io.h"
#include "image/j2k_stream.h"
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>
#include "fuzz_common.h"

using namespace satdump::image;

namespace
{
    // The pictures a decode may make, at most (width x height x components): enough for every
    // good codestream here, and it keeps a header that claims a huge picture from being decoded
    const uint64_t MAX_SAMPLES = 1 << 22;

    std::vector<uint8_t> saved(int depth, size_t width, size_t height, int channels)
    {
        Image img(depth, width, height, channels);
        for (int c = 0; c < channels; c++)
            for (size_t i = 0; i < width * height; i++)
                img.set(c, i, (int)(((uint32_t)i * 2654435761u + (uint32_t)c * 977u) >> (depth == 8 ? 24 : 16)));
        const std::filesystem::path path = std::filesystem::temp_directory_path() / ("satdump-fuzz-j2k-" + std::to_string(::getpid()) + ".j2k");
        save_j2k(img, path.string());
        std::ifstream f(path, std::ios::binary);
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        f.close();
        std::filesystem::remove(path);
        return data;
    }

    // Made once, on first use
    const std::vector<std::vector<uint8_t>> &good_codestreams()
    {
        static const std::vector<std::vector<uint8_t>> all = {
            saved(8, 40, 24, 1),  // grey, 5 resolutions
            saved(16, 33, 17, 3), // colour (with the colour transform), 16 bits
            saved(8, 7, 5, 1),    // under 16 pixels: fewer resolutions
            saved(16, 20, 18, 4), // four components
        };
        return all;
    }
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 1)
        return 0;
    const uint8_t mode = data[0];
    data++, size--;

    std::vector<uint8_t> input;
    if ((mode & 1) == 0)
        input.assign(data, data + size);
    else
    {
        if (size < 4)
            return 0;
        const std::vector<std::vector<uint8_t>> &all = good_codestreams();
        const std::vector<uint8_t> &good = all[(mode >> 1) % all.size()];
        size_t keep = (size_t)data[0] << 8 | data[1];
        if (keep > good.size())
            keep = good.size();
        input.assign(good.begin(), good.begin() + keep);
        const size_t at = (size_t)data[2] << 8 | data[3];
        for (size_t i = 4; i < size && at + (i - 4) < input.size(); i++)
            input[at + (i - 4)] ^= data[i];
    }

    // Exactly the input's bytes in an allocation of their own, so any read past the end is caught
    const size_t n = input.size();
    std::unique_ptr<uint8_t[]> buf(new uint8_t[n == 0 ? 1 : n]);
    if (n > 0)
        memcpy(buf.get(), input.data(), n);

    // From memory, as decompress_j2k_openjp2 does
    Image first = j2k_decompress_buffer(buf.get(), n, MAX_SAMPLES);

    // Every component, as load_j2k does, and the first one again from the same decode
    J2KBuffer b;
    b.data = buf.get();
    b.size = n;
    opj_stream_t *stream = j2k_create_buffer_stream(&b);
    std::string error, warning;
    opj_image_t *decoded = j2k_decode(stream, error, warning, MAX_SAMPLES);
    if (stream != nullptr)
        opj_stream_destroy(stream);
    if (decoded != nullptr)
    {
        Image all;
        if (j2k_all_components(decoded, all, error))
        {
            // The picture's size is the first component's size
            if (all.width() != decoded->comps[0].w || all.height() != decoded->comps[0].h)
                __builtin_trap();
        }
        Image again = j2k_first_component(decoded);
        // A decode is repeatable: the same data gives the same first component
        if (again.size() != first.size())
            __builtin_trap();
        for (size_t i = 0; i < again.size(); i++)
            if (again.get(i) != first.get(i))
                __builtin_trap();
        opj_image_destroy(decoded);
    }
    else if (first.size() != 0)
        __builtin_trap(); // one decode failed and the other did not
    return 0;
}
