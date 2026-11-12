// Fork: the memory stream SatDump decodes J2K data through (src-core/image/j2k_stream.h). The
// system OpenJPEG has no memory stream; the bundled copy had opj_stream_create_buffer_stream,
// whose callbacks these replace. They give what the bundled ones gave, except after a skip past
// the end, where the bundled skip left the position past the end and the next read copied bytes
// from after the buffer (OpenJPEG never asks for that, as it knows the length).

#include "doctest.h"
#include "image/io.h"
#include "image/j2k_stream.h"
#include "log_capture.h"
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace satdump::image;

namespace
{
    const OPJ_SIZE_T READ_END = (OPJ_SIZE_T)-1;

    // A 40 x 24 8-bit picture saved as .j2k, read back as bytes
    std::vector<uint8_t> j2k_bytes(Image &img)
    {
        const std::filesystem::path path = std::filesystem::temp_directory_path() / ("satdump-j2k-stream-" + std::to_string(::getpid()) + ".j2k");
        save_j2k(img, path.string());
        std::ifstream f(path, std::ios::binary);
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        f.close();
        std::filesystem::remove(path);
        return data;
    }

    Image test_picture()
    {
        Image img(8, 40, 24, 1);
        for (size_t i = 0; i < img.size(); i++)
            img.set(i, (int)((i * 37 + i / 40 * 11) & 0xFF));
        return img;
    }
} // namespace

TEST_CASE("J2K memory stream: reads stop at the end, and a read at the end gives -1")
{
    const uint8_t bytes[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    J2KBuffer b;
    b.data = bytes;
    b.size = sizeof(bytes);
    uint8_t out[32];

    CHECK(j2k_buffer_read(out, 4, &b) == 4);
    CHECK(memcmp(out, bytes, 4) == 0);
    CHECK(b.pos == 4);
    CHECK(j2k_buffer_read(out, 0, &b) == 0);
    CHECK(b.pos == 4);
    CHECK(j2k_buffer_read(out, 100, &b) == 6); // past the end: what is left
    CHECK(memcmp(out, bytes + 4, 6) == 0);
    CHECK(b.pos == 10);
    CHECK(j2k_buffer_read(out, 1, &b) == READ_END);
    CHECK(b.pos == 10);
}

TEST_CASE("J2K memory stream: skips stop at the end (the bundled skip went past it)")
{
    const uint8_t bytes[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    J2KBuffer b;
    b.data = bytes;
    b.size = sizeof(bytes);
    uint8_t out[32];

    CHECK(j2k_buffer_skip(0, &b) == 0);
    CHECK(j2k_buffer_skip(4, &b) == 4);
    CHECK(b.pos == 4);
    CHECK(j2k_buffer_read(out, 1, &b) == 1);
    CHECK(out[0] == 4);
    CHECK(j2k_buffer_skip(-2, &b) == -1); // OpenJPEG never skips backwards
    CHECK(b.pos == 5);
    CHECK(j2k_buffer_skip(100, &b) == 5); // past the end: what is left (the same as before)...
    CHECK(b.pos == 10);                   // ...but the position stays at the end (was 105)
    CHECK(j2k_buffer_read(out, 1, &b) == READ_END); // (copied a byte from after the buffer)
    CHECK(j2k_buffer_skip(1, &b) == -1);
}

TEST_CASE("J2K memory stream: seeks past the end or before the start go to the end and succeed")
{
    const uint8_t bytes[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    J2KBuffer b;
    b.data = bytes;
    b.size = sizeof(bytes);
    uint8_t out[32];

    CHECK(j2k_buffer_seek(3, &b) == OPJ_TRUE);
    CHECK(b.pos == 3);
    CHECK(j2k_buffer_read(out, 2, &b) == 2);
    CHECK(out[0] == 3);
    CHECK(out[1] == 4);
    CHECK(j2k_buffer_seek(10, &b) == OPJ_TRUE);
    CHECK(b.pos == 10);
    CHECK(j2k_buffer_seek(0, &b) == OPJ_TRUE);
    CHECK(b.pos == 0);
    CHECK(j2k_buffer_seek(11, &b) == OPJ_TRUE);
    CHECK(b.pos == 10);
    CHECK(j2k_buffer_seek(-1, &b) == OPJ_TRUE);
    CHECK(b.pos == 10);
}

TEST_CASE("J2K memory stream: an empty buffer reads and skips nothing")
{
    J2KBuffer b;
    uint8_t out[4];
    CHECK(j2k_buffer_read(out, 4, &b) == READ_END);
    CHECK(j2k_buffer_skip(4, &b) == -1);
    CHECK(j2k_buffer_seek(0, &b) == OPJ_TRUE);
    CHECK(j2k_buffer_seek(5, &b) == OPJ_TRUE);
    CHECK(b.pos == 0);

    // Through OpenJPEG: no header, so an error and no picture
    opj_stream_t *stream = j2k_create_buffer_stream(&b);
    REQUIRE(stream != nullptr);
    std::string error, warning;
    opj_image_t *decoded = j2k_decode(stream, error, warning);
    opj_stream_destroy(stream);
    CHECK(decoded == nullptr);
    CHECK(error.find("could not read the header") == 0);
}

TEST_CASE("J2K memory stream: no buffer, or no data with a size, gives no stream")
{
    CHECK(j2k_create_buffer_stream(nullptr) == nullptr);
    J2KBuffer b;
    b.size = 10;
    CHECK(j2k_create_buffer_stream(&b) == nullptr);

    std::string error, warning;
    CHECK(j2k_decode(nullptr, error, warning) == nullptr);
    CHECK(error == "could not open the input");
}

TEST_CASE("J2K memory stream: a codestream decodes to the picture it was saved from")
{
    Image img = test_picture();
    std::vector<uint8_t> data = j2k_bytes(img);
    REQUIRE(data.size() > 100);

    J2KBuffer b;
    b.data = data.data();
    b.size = data.size();
    opj_stream_t *stream = j2k_create_buffer_stream(&b);
    REQUIRE(stream != nullptr);
    std::string error, warning;
    opj_image_t *decoded = j2k_decode(stream, error, warning);
    opj_stream_destroy(stream);
    REQUIRE(decoded != nullptr);
    CHECK(error.empty());
    CHECK(warning.empty());

    Image all;
    CHECK(j2k_all_components(decoded, all, error));
    Image first = j2k_first_component(decoded);
    opj_image_destroy(decoded);

    REQUIRE(all.width() == 40);
    REQUIRE(all.height() == 24);
    CHECK(all.depth() == 8);
    REQUIRE(first.width() == 40);
    REQUIRE(first.height() == 24);
    CHECK(first.depth() == 16);
    bool same = true;
    for (size_t i = 0; i < img.size(); i++)
        same = same && all.get(i) == img.get(i) && first.get(i) == img.get(i);
    CHECK(same);

    // The same through j2k_decompress_buffer, which decompress_j2k_openjp2 calls
    Image mem = j2k_decompress_buffer(data.data(), data.size());
    REQUIRE(mem.size() == img.size());
    same = true;
    for (size_t i = 0; i < img.size(); i++)
        same = same && mem.get(i) == img.get(i);
    CHECK(same);
}

TEST_CASE("J2K memory stream: a picture larger than the limit is refused before it is decoded")
{
    Image img = test_picture();
    std::vector<uint8_t> data = j2k_bytes(img);

    test_log::LogCapture log;
    CHECK(j2k_decompress_buffer(data.data(), data.size(), 40 * 24 - 1).size() == 0);
    CHECK(log.has(slog::LOG_WARN, "larger than allowed"));
    CHECK(j2k_decompress_buffer(data.data(), data.size(), 40 * 24).size() == 40 * 24);
}

TEST_CASE("J2K memory stream: OpenJPEG's reason is logged for data it cannot decode")
{
    std::vector<uint8_t> garbage(64, 0x42);
    test_log::LogCapture log;
    CHECK(j2k_decompress_buffer(garbage.data(), garbage.size()).size() == 0);
    // "could not read the header: <OpenJPEG's message>"
    CHECK(log.has(slog::LOG_WARN, "J2K: could not decompress 64 bytes of data: could not read the header: "));
}
