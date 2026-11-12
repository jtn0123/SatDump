// Fork: decompress_jpeg (image/jpeg_utils.cpp), the 8-bit grey decoder for picture data in
// radio streams (Elektro-L LRIT, MSG SEVIRI, GK-2A LRIT, PROBA), with the copy of IJG libjpeg 6b
// in libs/jpeg (from DCMTK). test_jpeg8.cpp pins its results on good, cut-off and damaged
// streams; these tests add the encoder settings the copy has (progressive, restart markers,
// lossless with each predictor, colour) and the ways radio data can be damaged.
//
// With ignore_errors, an error once the headers are read keeps the rows decoded so far and leaves
// the rest 0; an error in the headers, or without ignore_errors, gives no picture. Before,
// ignore_errors let libjpeg carry on after an error, which it is not written to do.
#include "doctest.h"
#include "image/image.h"
#include "image/jpeg_utils.h"
#include "jpeg_test_check.h"
#include "jpeg_test_util.h"
#include "log_capture.h"
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

    // The good-data entries were recorded with the bundled 8-bit copy before the fork changed
    // decompress_jpeg, the damaged-data ones with the fork's decompress_jpeg
    const Expected EXPECTED[] = {
        // Good data, recorded before the fork changed decompress_jpeg
        {"grey 33x19 q75 bytes", 0x78b74622bc930bd4ULL},
        {"grey 1x1 q75 bytes", 0xfdc2d04c77872608ULL},
        {"grey 7x5 q90 bytes", 0xe4c3a7e18a3fea15ULL},
        {"grey 64x48 q95 optimized bytes", 0x6fb867b94bb10bdcULL},
        {"grey 33x19 progressive bytes", 0x8ad6edec10d54204ULL},
        {"grey 64x48 q50 progressive bytes", 0xe757537123e3f76aULL},
        {"grey 33x19 restart every MCU bytes", 0xd01f7e74d4f54240ULL},
        {"grey 33x19 lossless p1 bytes", 0x3f3a117e560329a3ULL},
        {"grey 33x19 lossless p2 bytes", 0xf514aa33056bff67ULL},
        {"grey 33x19 lossless p3 bytes", 0xbf84f3969430a396ULL},
        {"grey 33x19 lossless p4 bytes", 0x5adf169cfdfba0f4ULL},
        {"grey 33x19 lossless p5 bytes", 0x218cdab0b1c8441eULL},
        {"grey 33x19 lossless p6 bytes", 0x6940d8e9df9dc6afULL},
        {"grey 33x19 lossless p7 bytes", 0x2aaaeec386521e3cULL},
        {"grey 33x19 lossless p1 pt2 bytes", 0xf1f83f8b4a9ac78bULL},
        {"grey 33x19 lossless p6 restart every row bytes", 0x4b9541f0e6cf29e9ULL},
        {"ycc420 33x19 bytes", 0x5ffbd0535cfe5e38ULL},
        {"ycc444 33x19 bytes", 0x4ef459484be34523ULL},
        {"rgb 33x19 bytes", 0x11aff6f3eac03efaULL},
        {"cmyk 33x19 bytes", 0xdab420de71e87f8aULL},
        {"grey 33x19 q75 pixels", 0xff3c1083e8f92ed2ULL},
        {"grey 1x1 q75 pixels", 0x27ff636b97f4de0fULL},
        {"grey 7x5 q90 pixels", 0xb3e1a257290535f1ULL},
        {"grey 64x48 q95 optimized pixels", 0x627d1a2726714d78ULL},
        {"grey 33x19 progressive pixels", 0xff3c1083e8f92ed2ULL},
        {"grey 64x48 q50 progressive pixels", 0x2dfecbaa4245f7b0ULL},
        {"grey 33x19 restart every MCU pixels", 0xff3c1083e8f92ed2ULL},
        {"grey 33x19 lossless p1 pixels", 0x6d8e3bd2e81727ddULL},
        {"grey 33x19 lossless p2 pixels", 0x6d8e3bd2e81727ddULL},
        {"grey 33x19 lossless p3 pixels", 0x6d8e3bd2e81727ddULL},
        {"grey 33x19 lossless p4 pixels", 0x6d8e3bd2e81727ddULL},
        {"grey 33x19 lossless p5 pixels", 0x6d8e3bd2e81727ddULL},
        {"grey 33x19 lossless p6 pixels", 0x6d8e3bd2e81727ddULL},
        {"grey 33x19 lossless p7 pixels", 0x6d8e3bd2e81727ddULL},
        {"grey 33x19 lossless p1 pt2 pixels", 0x3ec8b9445487ac78ULL},
        {"grey 33x19 lossless p6 restart every row pixels", 0x6d8e3bd2e81727ddULL},
        {"grey 33x19 q75 cut at 60% pixels", 0xeb407b9d818237c1ULL},
        {"grey 1x1 q75 cut at 60% pixels", 0x27ffc56b97f58495ULL},
        {"grey 7x5 q90 cut at 60% pixels", 0x0706a41e02056ef8ULL},
        {"grey 64x48 q95 optimized cut at 60% pixels", 0xc47a541c0b5f4619ULL},
        {"grey 33x19 progressive cut at 60% pixels", 0x35671dc57e35e15fULL},
        {"grey 64x48 q50 progressive cut at 60% pixels", 0xb79660577b78fc82ULL},
        {"grey 33x19 restart every MCU cut at 60% pixels", 0x25fdca13e7e7b675ULL},
        {"grey 33x19 lossless p1 cut at 60% pixels", 0x81276843e647561fULL},
        {"grey 33x19 lossless p2 cut at 60% pixels", 0xf938da3c7b6803e3ULL},
        {"grey 33x19 lossless p3 cut at 60% pixels", 0xbde12687f11f2cf0ULL},
        {"grey 33x19 lossless p4 cut at 60% pixels", 0xc63abf6de7337481ULL},
        {"grey 33x19 lossless p5 cut at 60% pixels", 0x5217a1d57ce31e93ULL},
        {"grey 33x19 lossless p6 cut at 60% pixels", 0x59d7781c1828330fULL},
        {"grey 33x19 lossless p7 cut at 60% pixels", 0x5457b46ab053d43bULL},
        {"grey 33x19 lossless p1 pt2 cut at 60% pixels", 0x78fa030a9077e270ULL},
        {"grey 33x19 lossless p6 restart every row cut at 60% pixels", 0x57b98c3e64632b9bULL},
        // Damaged data, recorded with the fork's decompress_jpeg
        {"grey 33x19 q75 every cut", 0xecd8e7a60fa240a0ULL},
        {"grey 33x19 q75 every cut (ignore_errors)", 0x70be527f9ee28425ULL},
        {"grey 33x19 progressive every cut", 0xdbd121e8fd8482e9ULL},
        {"grey 33x19 progressive every cut (ignore_errors)", 0x239d380153f87a61ULL},
        {"grey 33x19 lossless p1 every cut", 0xa08db3a30b5e2b2dULL},
        {"grey 33x19 lossless p1 every cut (ignore_errors)", 0x1b68eadd0f666595ULL},
        {"ycc420 33x19 decompress_jpeg", 0xe88f6337729fede0ULL},
        {"ycc420 33x19 decompress_jpeg(ignore_errors)", 0xe88f6337729fede0ULL},
        {"ycc444 33x19 decompress_jpeg", 0xe88f6337729fede0ULL},
        {"ycc444 33x19 decompress_jpeg(ignore_errors)", 0xe88f6337729fede0ULL},
        {"rgb 33x19 decompress_jpeg", 0x171bb79dc8e18503ULL},
        {"rgb 33x19 decompress_jpeg(ignore_errors)", 0x1761bbce0ad6a768ULL},
        {"cmyk 33x19 decompress_jpeg", 0x171bb79dc8e18503ULL},
        {"cmyk 33x19 decompress_jpeg(ignore_errors)", 0x1761bbce0ad6a768ULL},
        {"grey 33x19 q75 DC symbols 16", 0xe9ab40f5e4b88abdULL},
        {"grey 33x19 q75 DC symbols 16 (ignore_errors)", 0xe9ab40f5e4b88abdULL},
        {"grey 33x19 q75 DC symbols 17", 0x171bb79dc8e18503ULL},
        {"grey 33x19 q75 DC symbols 17 (ignore_errors)", 0x1761bbce0ad6a768ULL},
        {"grey 33x19 q75 DC symbols 200", 0x171bb79dc8e18503ULL},
        {"grey 33x19 q75 DC symbols 200 (ignore_errors)", 0x1761bbce0ad6a768ULL},
        {"grey 33x19 progressive DC symbols 16", 0x39d73e39b54079c3ULL},
        {"grey 33x19 progressive DC symbols 16 (ignore_errors)", 0x39d73e39b54079c3ULL},
        {"grey 33x19 progressive DC symbols 17", 0x171bb79dc8e18503ULL},
        {"grey 33x19 progressive DC symbols 17 (ignore_errors)", 0x1761bbce0ad6a768ULL},
        {"grey 33x19 progressive DC symbols 200", 0x171bb79dc8e18503ULL},
        {"grey 33x19 progressive DC symbols 200 (ignore_errors)", 0x1761bbce0ad6a768ULL},
        {"grey 33x19 lossless p1 DC symbols 16", 0x4541cb0355feaee8ULL},
        {"grey 33x19 lossless p1 DC symbols 16 (ignore_errors)", 0x4541cb0355feaee8ULL},
        {"grey 33x19 lossless p1 DC symbols 17", 0x171bb79dc8e18503ULL},
        {"grey 33x19 lossless p1 DC symbols 17 (ignore_errors)", 0x1761bbce0ad6a768ULL},
        {"grey 33x19 lossless p1 DC symbols 200", 0x171bb79dc8e18503ULL},
        {"grey 33x19 lossless p1 DC symbols 200 (ignore_errors)", 0x1761bbce0ad6a768ULL},
    };

    std::vector<uint8_t> make(const EncodeSettings &s) { return encode(s, test_samples(s.width, s.height, s.components, 255)); }

    Image decode(const std::vector<uint8_t> &data, bool ignore_errors)
    {
        std::vector<uint8_t> copy = data; // decompress_jpeg takes a non-const pointer
        return satdump::image::decompress_jpeg(copy.data(), (int)copy.size(), ignore_errors);
    }
} // namespace

TEST_CASE("JPEG 8-bit: the bundled encoder makes the same streams as before")
{
    for (auto &n : grey_settings())
    {
        std::vector<uint8_t> data = make(n.s);
        REQUIRE(!data.empty());
        check(EXPECTED, std::string(n.name) + " bytes", bytes_hash(data));
    }
    for (auto &n : colour_settings())
    {
        std::vector<uint8_t> data = make(n.s);
        REQUIRE(!data.empty());
        check(EXPECTED, std::string(n.name) + " bytes", bytes_hash(data));
    }
}

TEST_CASE("JPEG 8-bit: decompress_jpeg gives the same pixels as before")
{
    for (auto &n : grey_settings())
    {
        std::vector<uint8_t> data = make(n.s);
        Image a = decode(data, false);
        REQUIRE(a.width() == (size_t)n.s.width);
        REQUIRE(a.height() == (size_t)n.s.height);
        CHECK(a.depth() == 8);
        check(EXPECTED, std::string(n.name) + " pixels", image_hash(a));
        Image b = decode(data, true);
        CHECK(image_hash(b) == image_hash(a));
    }
}

TEST_CASE("JPEG 8-bit: lossless streams decode to exactly the samples encoded")
{
    for (auto &n : grey_settings())
    {
        if (n.s.lossless_predictor == 0 || n.s.point_transform != 0)
            continue;
        INFO(std::string(n.name));
        std::vector<int> samples = test_samples(n.s.width, n.s.height, 1, 255);
        Image img = decode(make(n.s), false);
        REQUIRE(img.size() == samples.size());
        bool same = true;
        for (size_t i = 0; i < samples.size(); i++)
            same = same && img.get(i) == samples[i];
        CHECK(same);
    }
}

TEST_CASE("JPEG 8-bit: a stream cut off in its picture data decodes as before")
{
    // libjpeg warns and fills the rest; that is not an error, so ignore_errors changes nothing
    for (auto &n : grey_settings())
    {
        std::vector<uint8_t> data = make(n.s);
        size_t at = data.size() * 6 / 10; // as test_jpeg8.cpp, when that is in the picture data
        if (at <= first_scan_data(data))
            at = first_scan_data(data) + 1;
        Image a = decode(cut(data, at), false);
        Image b = decode(cut(data, at), true);
        check(EXPECTED, std::string(n.name) + " cut at 60% pixels", image_hash(a));
        CHECK(image_hash(b) == image_hash(a));
    }
}

TEST_CASE("JPEG 8-bit: cut off anywhere, the results are pinned")
{
    // Every length of a sequential, a progressive and a lossless stream. Cut in the headers,
    // libjpeg finds an error (no picture; with ignore_errors, a black one once the size is
    // known); cut in the picture data, it warns and fills the rest. The sanitizer build checks
    // that no length makes it read or write out of bounds.
    for (const char *name : {"grey 33x19 q75", "grey 33x19 progressive", "grey 33x19 lossless p1"})
    {
        std::vector<uint8_t> data = make(grey_setting(name));
        Hash strict, ignoring;
        for (size_t len = 0; len <= data.size(); len++)
        {
            Image a = decode(cut(data, len), false);
            Image b = decode(cut(data, len), true);
            strict.u64(image_hash(a));
            ignoring.u64(image_hash(b));
        }
        check(EXPECTED, std::string(name) + " every cut", strict.h);
        check(EXPECTED, std::string(name) + " every cut (ignore_errors)", ignoring.h);
    }
}

TEST_CASE("JPEG 8-bit: a stream with three components gives its first component only")
{
    // The output buffer holds one sample per pixel; libjpeg used to write three (red, green and
    // blue) into it, past its end. Grey output is now asked for, which is the luminance of a
    // YCbCr stream. RGB and CMYK streams cannot be turned into grey by this libjpeg, and are
    // refused (with ignore_errors: a black picture of their size).
    for (auto &n : colour_settings())
    {
        std::vector<uint8_t> data = make(n.s);
        Image a = decode(data, false);
        Image b = decode(data, true);
        check(EXPECTED, std::string(n.name) + " decompress_jpeg", image_hash(a));
        check(EXPECTED, std::string(n.name) + " decompress_jpeg(ignore_errors)", image_hash(b));
        if (n.s.components == 3 && !n.s.rgb)
        {
            CHECK(a.width() == 33);
            CHECK(a.height() == 19);
            CHECK(a.channels() == 1);
            CHECK(image_hash(b) == image_hash(a));
        }
        else
        {
            CHECK(a.size() == 0);
            CHECK(b.width() == 33);
            CHECK(b.height() == 19);
            CHECK(all_zero(b));
        }
    }
}

TEST_CASE("JPEG 8-bit: a header claiming a huge picture is refused before decoding")
{
    // 10001 x 10000 is just over the limit of 100 million pixels: refused with a warning, in
    // both modes, before any memory is set aside for it
    std::vector<uint8_t> data = make(settings(33, 19, 75));
    test_log::LogCapture log;
    for (bool ignore : {false, true})
    {
        Image img = decode(with_size(data, 10001, 10000), ignore);
        CHECK(img.size() == 0);
    }
    CHECK(log.has(slog::LOG_WARN, "100010000"));
}

TEST_CASE("JPEG 8-bit: a progressive stream with a damaged second scan")
{
    // The second scan asks for coefficients past the end of a block (Se = 255). libjpeg finds
    // this while reading the whole stream, before any row is decoded: no picture, or with
    // ignore_errors a black one of the right size. Before, ignore_errors let libjpeg go on and
    // write past its coefficient blocks.
    std::vector<uint8_t> data = make(grey_setting("grey 33x19 progressive"));
    size_t sos = nth_sos(data, 1);
    REQUIRE(sos != 0);
    size_t ns = data[sos + 4];
    data[sos + 5 + 2 * ns + 1] = 0xFF; // Se
    test_log::LogCapture log;
    Image a = decode(data, false);
    CHECK(a.size() == 0);
    Image b = decode(data, true);
    CHECK(b.width() == 33);
    CHECK(b.height() == 19);
    CHECK(all_zero(b));
    CHECK(log.has(slog::LOG_WARN, "0 of 19 rows"));
}

TEST_CASE("JPEG 8-bit: a DC Huffman symbol of 16 or more is bad data, not a crash")
{
    // The copy accepts DC symbols up to 16 in a table and refuses larger ones (an error once the
    // size is known). A sequential or progressive scan has no 16: the decoder read past the end of
    // its 16-entry tables. That is now treated as a bad Huffman code: a warning, and a difference
    // of 0. In a lossless scan, 16 is allowed and means a difference of 32768.
    for (const char *name : {"grey 33x19 q75", "grey 33x19 progressive", "grey 33x19 lossless p1"})
    {
        std::vector<uint8_t> good = make(grey_setting(name));
        for (int sym : {16, 17, 200})
        {
            std::vector<uint8_t> bad = with_dc_symbols(good, (uint8_t)sym);
            REQUIRE(bad != good);
            Image a = decode(bad, false);
            Image b = decode(bad, true);
            check(EXPECTED, std::string(name) + " DC symbols " + std::to_string(sym), image_hash(a));
            check(EXPECTED, std::string(name) + " DC symbols " + std::to_string(sym) + " (ignore_errors)", image_hash(b));
        }
    }
}

TEST_CASE("JPEG 8-bit: a scan naming a Huffman table that cannot exist is bad data")
{
    // A scan header names each component's tables with 4 bits (0 to 15), and there are 4 tables.
    // The number indexed the decoder's 4 tables before libjpeg checked it; it is now checked
    // first, with the same error: no picture, or with ignore_errors a black one of the right size.
    struct Case
    {
        const char *name;
        int scan;       // the scan whose header is changed
        uint8_t tables; // DC table number in the top 4 bits, AC in the bottom 4
    };
    const Case cases[] = {
        {"grey 33x19 q75", 0, 0x40},
        {"grey 33x19 q75", 0, 0x04},
        {"grey 33x19 q75", 0, 0xFF},
        {"grey 33x19 progressive", 0, 0x40}, // the first scan: DC only
        {"grey 33x19 progressive", 1, 0x04}, // the second: AC only
        {"grey 33x19 progressive", 1, 0xFF},
    };
    for (auto &c : cases)
    {
        std::vector<uint8_t> data = make(grey_setting(c.name));
        size_t sos = nth_sos(data, c.scan);
        REQUIRE(sos != 0);
        REQUIRE(data[sos + 4] == 1); // one component
        data[sos + 6] = c.tables;
        std::string what = std::string(c.name) + ", scan " + std::to_string(c.scan) + ", tables " + std::to_string(c.tables);
        INFO(what);
        Image a = decode(data, false);
        Image b = decode(data, true);
        CHECK(a.size() == 0);
        CHECK(b.width() == 33);
        CHECK(b.height() == 19);
        CHECK(all_zero(b));
    }
}

TEST_CASE("JPEG 8-bit: a Huffman table numbered over 3 is refused")
{
    // A table definition gives its class (DC or AC) and number in one byte, and there are 4
    // tables of each class. The number indexed them before libjpeg checked it (found by
    // fuzz_jpeg); the same error now comes first. It is in the headers: no picture, in both modes.
    std::vector<uint8_t> data = make(settings(33, 19, 75));
    size_t dht = find_marker(data, 0xC4);
    REQUIRE(dht != 0);
    for (uint8_t id : {0x04, 0x0F, 0x14, 0xFF}) // DC 4 and 15, AC 4 and 239
    {
        std::vector<uint8_t> bad = data;
        bad[dht + 4] = id;
        std::string what = "table byte " + std::to_string(id);
        INFO(what);
        CHECK(decode(bad, false).size() == 0);
        CHECK(decode(bad, true).size() == 0);
    }
}

TEST_CASE("JPEG 8-bit: a lossless stream with an impossible precision or point transform is refused")
{
    // Lossless JPEG has 2 to 16 bits per sample, and its point transform is below that. Any
    // precision and any point transform up to 15 were accepted, and the first row's predictor,
    // 1 << (precision - point transform - 1), was then shifted by a negative number or by more
    // than an int holds (found by fuzz_jpeg). They are now refused: the precision while the
    // headers are read (no picture in both modes), the point transform once decoding starts
    // (with ignore_errors, a black picture of the right size).
    std::vector<uint8_t> data = make(grey_setting("grey 33x19 lossless p1"));
    size_t sof = find_sof(data);
    REQUIRE(sof != 0);
    REQUIRE(data[sof + 1] == 0xC3);
    for (int precision : {0, 1, 17, 255})
    {
        std::vector<uint8_t> bad = data;
        bad[sof + 4] = (uint8_t)precision;
        std::string what = "precision " + std::to_string(precision);
        INFO(what);
        CHECK(decode(bad, false).size() == 0);
        CHECK(decode(bad, true).size() == 0);
    }
    size_t sos = nth_sos(data, 0);
    REQUIRE(sos != 0);
    size_t ahal = sos + 5 + 2 * data[sos + 4] + 2;
    for (int pt : {8, 15})
    {
        std::vector<uint8_t> bad = data;
        bad[ahal] = (uint8_t)pt; // Ah 0, Al the point transform
        std::string what = "point transform " + std::to_string(pt);
        INFO(what);
        CHECK(decode(bad, false).size() == 0);
        Image b = decode(bad, true);
        CHECK(b.width() == 33);
        CHECK(b.height() == 19);
        CHECK(all_zero(b));
    }
}

TEST_CASE("JPEG 8-bit: no data, a bad length or a stream with no picture gives no picture")
{
    std::vector<uint8_t> data = make(settings(33, 19, 75));
    CHECK(satdump::image::decompress_jpeg(data.data(), 0).size() == 0);
    CHECK(satdump::image::decompress_jpeg(data.data(), 0, true).size() == 0);
    // A caller's length that went below 0 must not be taken as a huge one
    CHECK(satdump::image::decompress_jpeg(data.data(), -100).size() == 0);
    CHECK(satdump::image::decompress_jpeg(data.data(), -100, true).size() == 0);
    CHECK(satdump::image::decompress_jpeg(nullptr, 100).size() == 0);
    CHECK(satdump::image::decompress_jpeg(nullptr, 100, true).size() == 0);
    // Not a JPEG stream at all
    std::vector<uint8_t> junk(64, 0x55);
    CHECK(decode(junk, false).size() == 0);
    CHECK(decode(junk, true).size() == 0);
    // Tables only (start, the headers before the frame header, end): a valid stream with no
    // picture in it
    size_t sof = find_sof(data);
    REQUIRE(sof != 0);
    std::vector<uint8_t> tables = cut(data, sof);
    tables.push_back(0xFF);
    tables.push_back(0xD9);
    CHECK(decode(tables, false).size() == 0);
    CHECK(decode(tables, true).size() == 0);
}

TEST_CASE("JPEG 8-bit: with ignore_errors, a stream given no picture is a warning saying why")
{
    // The caller with ignore_errors (MSG and Elektro-L xRIT) keeps whatever it gets, so libjpeg's
    // reason is a warning (it was not logged). For a strict caller, which decides itself (GK-2A
    // then tries JPEG 2000), it is a debug line.
    std::vector<uint8_t> junk(64, 0x55);
    {
        test_log::LogCapture log;
        CHECK(decode(junk, true).size() == 0);
        CHECK(log.has(slog::LOG_WARN, "Not a JPEG file"));
    }
    {
        test_log::LogCapture log;
        CHECK(decode(junk, false).size() == 0);
        CHECK(log.has(slog::LOG_DEBUG, "Not a JPEG file"));
        CHECK(!log.has(slog::LOG_WARN, "JPEG"));
    }
    {
        test_log::LogCapture log;
        CHECK(satdump::image::decompress_jpeg(junk.data(), 0, true).size() == 0);
        CHECK(log.has(slog::LOG_WARN, "no data"));
    }
}
