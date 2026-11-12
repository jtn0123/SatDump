// Fork: decompressMsgHritFileIfRequired (plugins/xrit_support/xrit/msg/decomp.cpp) on JPEG
// segments, as MSG and Elektro-L LRIT send them. It is the one caller of decompress_jpeg with
// ignore_errors, and replaces the segment's JPEG stream with the pixels it gets.
//
// A segment libjpeg gives no picture for keeps no image data, as before; it used to be reported
// as "ELEKTRO xRIT JPEG Depth should be 8!" with libjpeg's reason not logged at all, and is now a
// warning saying it was not decoded, after decompress_jpeg's warning giving libjpeg's reason.
#include "doctest.h"
#include "image/image.h"
#include "jpeg_test_util.h"
#include "log_capture.h"
#include "xrit/msg/decomp.h"
#include <cstdint>
#include <string>
#include <vector>
extern "C"
{
#include "libs/jpeg/jpeglib.h"
}
#include "jpeg_test_encoder.h"

namespace
{
    using satdump::image::Image;
    using namespace jpeg_test;

    // An image segment as the xRIT demuxer hands it on: a primary header and an image structure
    // record (8 bits per pixel, JPEG), then the JPEG stream
    satdump::xrit::XRITFile jpeg_segment(const std::vector<uint8_t> &jpeg, int width, int height)
    {
        const uint32_t header_length = 16 + 9;
        uint64_t data_bits = (uint64_t)jpeg.size() * 8;
        std::vector<uint8_t> d = {
            0, 0, 16, 0, // primary header: type 0, 16 bytes, file type 0 (image)
            (uint8_t)(header_length >> 24), (uint8_t)(header_length >> 16), (uint8_t)(header_length >> 8), (uint8_t)header_length,
        };
        for (int shift = 56; shift >= 0; shift -= 8)
            d.push_back((uint8_t)(data_bits >> shift));
        // Image structure record: type 1, 9 bytes, 8 bits per pixel, columns, lines, JPEG (2)
        d.insert(d.end(), {1, 0, 9, 8, (uint8_t)(width >> 8), (uint8_t)width, (uint8_t)(height >> 8), (uint8_t)height, 2});
        d.insert(d.end(), jpeg.begin(), jpeg.end());

        satdump::xrit::XRITFile file;
        file.lrit_data = d;
        file.parseHeaders();
        file.filename = "TEST-SEGMENT";
        return file;
    }

    // The segment's image data after decompressing, as a picture of the given size
    Image segment_pixels(satdump::xrit::XRITFile &file, int width, int height)
    {
        Image img(8, width, height, 1);
        REQUIRE(file.lrit_data.size() == 25 + (size_t)width * height);
        for (size_t i = 0; i < (size_t)width * height; i++)
            img.set(i, file.lrit_data[25 + i]);
        return img;
    }
} // namespace

TEST_CASE("MSG and Elektro xRIT: a JPEG segment is replaced by its pixels, as before")
{
    std::vector<uint8_t> jpeg = encode(settings(33, 19, 75), test_samples(33, 19, 1, 255));
    REQUIRE(!jpeg.empty());
    satdump::xrit::XRITFile file = jpeg_segment(jpeg, 33, 19);
    test_log::LogCapture log;
    satdump::xrit::msg::decompressMsgHritFileIfRequired(file);
    Image img = segment_pixels(file, 33, 19);
    // "grey 33x19 q75 pixels" in test_jpeg8_decompress.cpp, recorded before the fork
    CHECK(image_hash(img) == 0xff3c1083e8f92ed2ULL);
    CHECK(!log.has(slog::LOG_WARN, "JPEG"));
}

TEST_CASE("MSG and Elektro xRIT: a JPEG segment cut off in its picture data keeps its full size, as before")
{
    // libjpeg warns and fills the rest: not an error, so the picture is the one decompress_jpeg
    // gives with or without ignore_errors ("grey 33x19 q75 cut at 60% pixels")
    std::vector<uint8_t> jpeg = encode(settings(33, 19, 75), test_samples(33, 19, 1, 255));
    REQUIRE(!jpeg.empty());
    satdump::xrit::XRITFile file = jpeg_segment(cut(jpeg, jpeg.size() * 6 / 10), 33, 19);
    satdump::xrit::msg::decompressMsgHritFileIfRequired(file);
    Image img = segment_pixels(file, 33, 19);
    CHECK(image_hash(img) == 0xeb407b9d818237c1ULL);
}

TEST_CASE("MSG and Elektro xRIT: a JPEG segment libjpeg refuses is a warning saying why, with no image data")
{
    // A frame header claiming 9 bits per sample: libjpeg refuses it while reading the headers
    std::vector<uint8_t> jpeg = encode(settings(33, 19, 75), test_samples(33, 19, 1, 255));
    size_t sof = find_sof(jpeg);
    REQUIRE(sof != 0);
    jpeg[sof + 4] = 9;
    satdump::xrit::XRITFile file = jpeg_segment(jpeg, 33, 19);
    test_log::LogCapture log;
    satdump::xrit::msg::decompressMsgHritFileIfRequired(file);
    CHECK(file.lrit_data.size() == 25); // the headers only, as before
    CHECK(log.has(slog::LOG_WARN, "precision 9"));
    CHECK(log.has(slog::LOG_WARN, "TEST-SEGMENT not decoded"));
    CHECK(!log.has(slog::LOG_ERROR, "Depth should be 8"));
}
