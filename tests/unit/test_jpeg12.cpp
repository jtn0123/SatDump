// Fork: 12-bit JPEG (image/jpeg12_utils.cpp), decoded with the copy of IJG libjpeg 6b in
// libs/jpeg12 (from DCMTK), which SatDump uses for GK-2A, Hinode, MATS and DSCOVR EPIC pictures.
//
// Good data: the streams the copy's own encoder makes with every setting (extended sequential,
// progressive, restart markers, lossless with each predictor, colour) must stay the same bytes,
// and decompress_jpeg12 must give the same pixels for them, with and without ignore_errors. In
// the sanitizer build this also checks that the copy has no undefined behaviour on them (it
// shifted negative numbers left, and bits off the top of its bit buffers).
//
// Damaged data, as radio data can be: a stream with three components (the decoder wrote three
// times past the end of its buffer), a header claiming a huge picture, a progressive stream
// whose second scan is damaged, Huffman tables with impossible symbols, streams cut off
// anywhere. With ignore_errors, an error once the headers are read keeps the rows decoded so far
// and leaves the rest 0; an error in the headers, or without ignore_errors, gives no picture.
#include "doctest.h"
#include "image/image.h"
#include "image/jpeg12_utils.h"
#include "jpeg_test_check.h"
#include "jpeg_test_util.h"
#include "log_capture.h"
#include <cstdint>
#include <string>
#include <vector>
extern "C"
{
#include "libs/jpeg12/jpeglib12.h"
}
#include "jpeg_test_encoder.h"

namespace
{
    using satdump::image::Image;
    using namespace jpeg_test;

    // The good-data entries were recorded with the bundled 12-bit copy before the fork changed
    // it, the damaged-data ones with the fork's decompress_jpeg12
    const Expected EXPECTED[] = {
        // Good data, recorded before the fork changed the 12-bit copy or decompress_jpeg12
        {"grey 33x19 q75 bytes", 0xaa64029f97f8f010ULL},
        {"grey 1x1 q75 bytes", 0xbce90d803ade85f0ULL},
        {"grey 7x5 q90 bytes", 0x823467196298b6f6ULL},
        {"grey 64x48 q95 optimized bytes", 0x9fdcb4f164965fb2ULL},
        {"grey 33x19 progressive bytes", 0xb26b616306f1a28dULL},
        {"grey 64x48 q50 progressive bytes", 0xd57131000dea3fabULL},
        {"grey 33x19 restart every MCU bytes", 0xcf8e56fe6bf7d183ULL},
        {"grey 33x19 lossless p1 bytes", 0xb60d23a510fe8399ULL},
        {"grey 33x19 lossless p2 bytes", 0x56263ef2314aff5eULL},
        {"grey 33x19 lossless p3 bytes", 0xe5a93ae831d24d83ULL},
        {"grey 33x19 lossless p4 bytes", 0xdd76f533e4d447b0ULL},
        {"grey 33x19 lossless p5 bytes", 0x07d0042b6d3f0ddcULL},
        {"grey 33x19 lossless p6 bytes", 0x5d3ce36d983ad153ULL},
        {"grey 33x19 lossless p7 bytes", 0x5c3adc7438561a4eULL},
        {"grey 33x19 lossless p1 pt2 bytes", 0xe8f72f886151bdd9ULL},
        {"grey 33x19 lossless p6 restart every row bytes", 0x31bd693d4cd60c79ULL},
        {"ycc420 33x19 bytes", 0x25e8d19fab32d5a1ULL},
        {"ycc444 33x19 bytes", 0x80a4e789d379cbfcULL},
        {"rgb 33x19 bytes", 0x1b2ea89cdd3a39d7ULL},
        {"cmyk 33x19 bytes", 0xd312af7b477e9d07ULL},
        {"grey 33x19 q75 pixels", 0xd0256a5523f955deULL},
        {"grey 1x1 q75 pixels", 0x224e5abae1ec2bdbULL},
        {"grey 7x5 q90 pixels", 0x14ce00204eb1d32aULL},
        {"grey 64x48 q95 optimized pixels", 0xf2440d4be12d764aULL},
        {"grey 33x19 progressive pixels", 0xd0256a5523f955deULL},
        {"grey 64x48 q50 progressive pixels", 0xa33ba7fba31162e6ULL},
        {"grey 33x19 restart every MCU pixels", 0xd0256a5523f955deULL},
        {"grey 33x19 lossless p1 pixels", 0x73da1a50fadfd902ULL},
        {"grey 33x19 lossless p2 pixels", 0x73da1a50fadfd902ULL},
        {"grey 33x19 lossless p3 pixels", 0x73da1a50fadfd902ULL},
        {"grey 33x19 lossless p4 pixels", 0x73da1a50fadfd902ULL},
        {"grey 33x19 lossless p5 pixels", 0x73da1a50fadfd902ULL},
        {"grey 33x19 lossless p6 pixels", 0x73da1a50fadfd902ULL},
        {"grey 33x19 lossless p7 pixels", 0x73da1a50fadfd902ULL},
        {"grey 33x19 lossless p1 pt2 pixels", 0x324b5db08820beb2ULL},
        {"grey 33x19 lossless p6 restart every row pixels", 0x73da1a50fadfd902ULL},
        {"grey 33x19 q75 cut at 60% pixels", 0xfbd889578418ba8bULL},
        {"grey 1x1 q75 cut at 60% pixels", 0x24a45abae3e821abULL},
        {"grey 7x5 q90 cut at 60% pixels", 0x372c25059bd502c9ULL},
        {"grey 64x48 q95 optimized cut at 60% pixels", 0x6d97082818049230ULL},
        {"grey 33x19 progressive cut at 60% pixels", 0x237db772d85f8587ULL},
        {"grey 64x48 q50 progressive cut at 60% pixels", 0x3a3f318763acc7f0ULL},
        {"grey 33x19 restart every MCU cut at 60% pixels", 0xb4f49442f1fea551ULL},
        {"grey 33x19 lossless p1 cut at 60% pixels", 0x97f48574b0cd2116ULL},
        {"grey 33x19 lossless p2 cut at 60% pixels", 0x540484da73ce1497ULL},
        {"grey 33x19 lossless p3 cut at 60% pixels", 0xb7e53f2b642c6454ULL},
        {"grey 33x19 lossless p4 cut at 60% pixels", 0x4a254bc054a04509ULL},
        {"grey 33x19 lossless p5 cut at 60% pixels", 0xa432f4084c4cd57fULL},
        {"grey 33x19 lossless p6 cut at 60% pixels", 0x2e3e75cc81989f8bULL},
        {"grey 33x19 lossless p7 cut at 60% pixels", 0xa36b90b5bc317f92ULL},
        {"grey 33x19 lossless p1 pt2 cut at 60% pixels", 0x3f471000a8e6c847ULL},
        {"grey 33x19 lossless p6 restart every row cut at 60% pixels", 0xab1e93fef672b0a6ULL},
        // Damaged data, recorded with the fork's decompress_jpeg12
        {"grey 33x19 q75 every cut", 0xfcbcd3c1592d906bULL},
        {"grey 33x19 q75 every cut (ignore_errors)", 0xe726c55783a19a43ULL},
        {"grey 33x19 progressive every cut", 0x5afd72d2cbadd8c1ULL},
        {"grey 33x19 progressive every cut (ignore_errors)", 0xc6db47d57fe22639ULL},
        {"grey 33x19 lossless p1 every cut", 0x869e8256a2128434ULL},
        {"grey 33x19 lossless p1 every cut (ignore_errors)", 0x93452dc1e8a2f51cULL},
        {"ycc420 33x19 decompress_jpeg12", 0xb5104116a9e6e9a3ULL},
        {"ycc420 33x19 decompress_jpeg12(ignore_errors)", 0xb5104116a9e6e9a3ULL},
        {"ycc444 33x19 decompress_jpeg12", 0xb5104116a9e6e9a3ULL},
        {"ycc444 33x19 decompress_jpeg12(ignore_errors)", 0xb5104116a9e6e9a3ULL},
        {"rgb 33x19 decompress_jpeg12", 0x171bb79dc8e18503ULL},
        {"rgb 33x19 decompress_jpeg12(ignore_errors)", 0xfd86a36b4f0ca720ULL},
        {"cmyk 33x19 decompress_jpeg12", 0x171bb79dc8e18503ULL},
        {"cmyk 33x19 decompress_jpeg12(ignore_errors)", 0xfd86a36b4f0ca720ULL},
        {"grey 33x19 q75 DC symbols 16", 0x0afb8c9e53bcf2fdULL},
        {"grey 33x19 q75 DC symbols 17", 0x0afb8c9e53bcf2fdULL},
        {"grey 33x19 q75 DC symbols 200", 0x0afb8c9e53bcf2fdULL},
        {"grey 33x19 q75 DC symbols 255", 0x0afb8c9e53bcf2fdULL},
        {"grey 33x19 progressive DC symbols 16", 0xbf660ff4199d560eULL},
        {"grey 33x19 progressive DC symbols 17", 0xbf660ff4199d560eULL},
        {"grey 33x19 progressive DC symbols 200", 0xbf660ff4199d560eULL},
        {"grey 33x19 progressive DC symbols 255", 0xbf660ff4199d560eULL},
        {"grey 33x19 lossless p1 DC symbols 16", 0x02217d11fa187ca0ULL},
        {"grey 33x19 lossless p1 DC symbols 17", 0x02217d11fa187ca0ULL},
        {"grey 33x19 lossless p1 DC symbols 200", 0x02217d11fa187ca0ULL},
        {"grey 33x19 lossless p1 DC symbols 255", 0x02217d11fa187ca0ULL},
    };

    std::vector<uint8_t> make(const EncodeSettings &s) { return encode(s, test_samples(s.width, s.height, s.components, 4095)); }

    Image decode(const std::vector<uint8_t> &data, bool ignore_errors)
    {
        std::vector<uint8_t> copy = data; // decompress_jpeg12 takes a non-const pointer
        return satdump::image::decompress_jpeg12(copy.data(), (int)copy.size(), ignore_errors);
    }
} // namespace

TEST_CASE("JPEG 12-bit: the bundled encoder makes the same streams as before")
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

TEST_CASE("JPEG 12-bit: decompress_jpeg12 gives the same pixels as before")
{
    for (auto &n : grey_settings())
    {
        std::vector<uint8_t> data = make(n.s);
        Image a = decode(data, false);
        REQUIRE(a.width() == (size_t)n.s.width);
        REQUIRE(a.height() == (size_t)n.s.height);
        CHECK(a.depth() == 16);
        check(EXPECTED, std::string(n.name) + " pixels", image_hash(a));
        Image b = decode(data, true);
        CHECK(image_hash(b) == image_hash(a));
    }
}

TEST_CASE("JPEG 12-bit: lossless streams decode to exactly the samples encoded")
{
    // The pixels are the 12-bit samples moved to the top of 16 bits
    for (auto &n : grey_settings())
    {
        if (n.s.lossless_predictor == 0 || n.s.point_transform != 0)
            continue;
        INFO(std::string(n.name));
        std::vector<int> samples = test_samples(n.s.width, n.s.height, 1, 4095);
        Image img = decode(make(n.s), false);
        REQUIRE(img.size() == samples.size());
        bool same = true;
        for (size_t i = 0; i < samples.size(); i++)
            same = same && img.get(i) == samples[i] << 4;
        CHECK(same);
    }
}

TEST_CASE("JPEG 12-bit: a stream cut off in its picture data decodes as before")
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

TEST_CASE("JPEG 12-bit: cut off anywhere, the results are pinned")
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

TEST_CASE("JPEG 12-bit: a stream with three components gives its first component only")
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
        check(EXPECTED, std::string(n.name) + " decompress_jpeg12", image_hash(a));
        check(EXPECTED, std::string(n.name) + " decompress_jpeg12(ignore_errors)", image_hash(b));
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

TEST_CASE("JPEG 12-bit: a header claiming a huge picture is refused before decoding")
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

TEST_CASE("JPEG 12-bit: a progressive stream with a damaged second scan")
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

TEST_CASE("JPEG 12-bit: impossible DC Huffman symbols are bad data, not a crash")
{
    // DCMTK's copy accepts DC symbols above 16 in a table (it only traces them); the decoder then
    // read past the end of its 16-entry tables and shifted by more than the bit buffer holds. A
    // symbol that large is now treated as a bad Huffman code: a warning, and a difference of 0.
    for (const char *name : {"grey 33x19 q75", "grey 33x19 progressive", "grey 33x19 lossless p1"})
    {
        std::vector<uint8_t> good = make(grey_setting(name));
        for (int sym : {16, 17, 200, 255})
        {
            std::vector<uint8_t> bad = with_dc_symbols(good, (uint8_t)sym);
            REQUIRE(bad != good);
            Image a = decode(bad, false);
            Image b = decode(bad, true);
            check(EXPECTED, std::string(name) + " DC symbols " + std::to_string(sym), image_hash(a));
            CHECK(image_hash(b) == image_hash(a));
        }
    }
}

TEST_CASE("JPEG 12-bit: a scan naming a Huffman table that cannot exist is bad data")
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

TEST_CASE("JPEG 12-bit: a Huffman table numbered over 3 is refused")
{
    // A table definition gives its class (DC or AC) and number in one byte, and there are 4
    // tables of each class. The number indexed them before libjpeg checked it (found by
    // fuzz_jpeg12); the same error now comes first. It is in the headers: no picture, in both modes.
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

TEST_CASE("JPEG 12-bit: a lossless stream with an impossible precision or point transform is refused")
{
    // Lossless JPEG has 2 to 16 bits per sample, and its point transform is below that. Any
    // precision and any point transform up to 15 were accepted, and the first row's predictor,
    // 1 << (precision - point transform - 1), was then shifted by a negative number or by more
    // than an int holds (found by fuzz_jpeg12). They are now refused: the precision while the
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
    for (int pt : {12, 15})
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

TEST_CASE("JPEG 12-bit: a refused stream leaves none of libjpeg's memory behind")
{
    // On an error, decompress_jpeg12 did not release libjpeg's memory (a few kB each time).
    // The sanitizer build's leak check finds any left at the end of the run.
    std::vector<uint8_t> data = make(settings(33, 19, 75));
    std::vector<uint8_t> junk(64, 0x55);
    junk[0] = 0xFF;
    junk[1] = 0xD8; // a start, then nothing JPEG
    CHECK(decode(junk, false).size() == 0);
    CHECK(decode(cut(data, find_sof(data) + 4), false).size() == 0); // cut in the frame header
    // A DC table that cannot exist: an error once decoding has started, with the most memory
    // set aside
    std::vector<uint8_t> bad_table = data;
    bad_table[nth_sos(data, 0) + 6] = 0x40;
    CHECK(decode(bad_table, false).size() == 0);
}

TEST_CASE("JPEG 12-bit: no data, or a bad length, gives no picture")
{
    std::vector<uint8_t> data = make(settings(33, 19, 75));
    CHECK(satdump::image::decompress_jpeg12(data.data(), 0).size() == 0);
    CHECK(satdump::image::decompress_jpeg12(data.data(), 0, true).size() == 0);
    // A caller's length that went below 0 must not be taken as a huge one
    CHECK(satdump::image::decompress_jpeg12(data.data(), -100).size() == 0);
    CHECK(satdump::image::decompress_jpeg12(data.data(), -100, true).size() == 0);
    CHECK(satdump::image::decompress_jpeg12(nullptr, 100).size() == 0);
    CHECK(satdump::image::decompress_jpeg12(nullptr, 100, true).size() == 0);
    // Not a JPEG stream at all
    std::vector<uint8_t> junk(64, 0x55);
    CHECK(decode(junk, false).size() == 0);
    CHECK(decode(junk, true).size() == 0);
}

TEST_CASE("JPEG 12-bit: with ignore_errors, a stream given no picture is a warning saying why")
{
    // As decompress_jpeg: libjpeg's reason is a warning with ignore_errors (it was not logged),
    // a debug line for a strict caller, which decides itself (GK-2A then tries JPEG 2000)
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
        CHECK(satdump::image::decompress_jpeg12(junk.data(), 0, true).size() == 0);
        CHECK(log.has(slog::LOG_WARN, "no data"));
    }
}
