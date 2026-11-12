// Fork: fuzz SatDump's 8-bit JPEG decoding, decompress_jpeg in src-core/image/jpeg_utils.cpp, with
// the bundled libjpeg (src-core/libs/jpeg), as used for MSG and Elektro-L LRIT, GK-2A LRIT and
// PROBA-2 SWAP pictures. The first byte picks how the rest of the input is used:
//   bit 0        ignore_errors (set) or not (clear)
//   bit 1 clear  the rest is the stream
//   bit 1 set    one of the good streams made here with the bundled encoder (picked by the other
//                bits) is cut to the length in the next two bytes, and the bytes after the next two
//                (which give a position) are xor-ed over it from there, so the fuzzer gets past the
//                headers quickly and into the picture data
// Checks: the picture is empty, or one channel of 8 bits within the size limit; and when the
// stream decodes without ignore_errors, it decodes to the same picture with it.
// The fuzz_jpeg12.cpp harness is the same for the 12-bit copy.
#include "image/image.h"
#include "image/jpeg_utils.h"
#include "jpeg_test_util.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>
#include "fuzz_common.h"
extern "C"
{
#include "libs/jpeg/jpeglib.h"
}
#include "jpeg_test_encoder.h"

using satdump::image::Image;

namespace
{
    // Pictures over this many pixels are refused (decompress_jpeg's own limit is 100 million):
    // enough for every good stream here, and it keeps each run fast
    const size_t MAX_PIXELS = 1 << 18;

    // Made once, on first use: every grey setting and the colour ones
    const std::vector<std::vector<uint8_t>> &good_streams()
    {
        static const std::vector<std::vector<uint8_t>> all = []
        {
            std::vector<std::vector<uint8_t>> out;
            std::vector<jpeg_test::NamedSettings> named = jpeg_test::grey_settings();
            for (auto &n : jpeg_test::colour_settings())
                named.push_back(n);
            for (auto &n : named)
            {
                std::vector<uint8_t> s = jpeg_test::encode(n.s, jpeg_test::test_samples(n.s.width, n.s.height, n.s.components, 255));
                if (!s.empty())
                    out.push_back(s);
            }
            return out;
        }();
        return all;
    }

    // Decodes from a copy of exactly the stream's bytes in an allocation of their own, so any
    // read past the end is caught
    Image decode(const std::vector<uint8_t> &stream, bool ignore_errors)
    {
        const size_t n = stream.size();
        std::unique_ptr<uint8_t[]> buf(new uint8_t[n == 0 ? 1 : n]);
        if (n > 0)
            memcpy(buf.get(), stream.data(), n);
        return satdump::image::decompress_jpeg(buf.get(), (int)n, ignore_errors, MAX_PIXELS);
    }

    bool same(Image &a, Image &b)
    {
        if (a.width() != b.width() || a.height() != b.height() || a.channels() != b.channels() || a.depth() != b.depth())
            return false;
        return a.size() == 0 || memcmp(a.raw_data(), b.raw_data(), a.size() * a.typesize()) == 0;
    }
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 1)
        return 0;
    const uint8_t mode = data[0];
    data++, size--;

    std::vector<uint8_t> stream;
    if ((mode & 2) == 0)
        stream.assign(data, data + size);
    else
    {
        if (size < 4)
            return 0;
        const std::vector<std::vector<uint8_t>> &all = good_streams();
        const std::vector<uint8_t> &good = all[(mode >> 2) % all.size()];
        stream = jpeg_test::cut(good, (size_t)data[0] << 8 | data[1]);
        const size_t at = (size_t)data[2] << 8 | data[3];
        for (size_t i = 4; i < size && at + (i - 4) < stream.size(); i++)
            stream[at + (i - 4)] ^= data[i];
    }

    const bool ignore_errors = (mode & 1) != 0;
    Image img = decode(stream, ignore_errors);
    if (img.size() != 0)
    {
        if (img.depth() != 8 || img.channels() != 1 || img.width() == 0 || img.height() == 0 || img.width() * img.height() > MAX_PIXELS)
            __builtin_trap();
    }
    else if (img.width() != 0 || img.height() != 0)
        __builtin_trap(); // an empty picture is all empty

    // A stream that decodes without ignore_errors decodes the same with it
    if (!ignore_errors && img.size() != 0)
    {
        Image again = decode(stream, true);
        if (!same(img, again))
            __builtin_trap();
    }
    return 0;
}
