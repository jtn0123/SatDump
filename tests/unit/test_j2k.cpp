// Fork: J2K (JPEG 2000 codestream) pictures through SatDump's own functions: save_j2k and
// load_j2k (.j2k files) and decompress_j2k_openjp2 (GOES GRB, FY-4, GK-2A and ORB data held in
// memory). Linux builds use Debian's OpenJPEG, other builds the bundled copy
// (cmake/fork/codecs.cmake). Only the functions SatDump had before are used here, so the same
// file also builds against an older libsatdump_core.
//
// The unusual codestreams below were made once with OpenJPEG 2.5.3 (a picture that does not start
// at 0,0, a 20-bit one and a subsampled one); their samples are pattern_value() in raster order.

#include "doctest.h"
#include "image/image.h"
#include "image/io.h"
#include "image/j2k_utils.h"
#include "log_capture.h"
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

using satdump::image::Image;

namespace
{
    namespace hex
    {
        // offset_8bit_20x12_at_3_2: 358 bytes
        const char *const offset_8bit_20x12_at_3_2 =
            "ff4fff5100290000000000170000000e0000000300000002000000170000000e00000000000000000001070101ff5200"
            "0c00000001000104040001ff5c00074040484850ff640025000143726561746564206279204f70656e4a504547207665"
            "7273696f6e20322e352e33ff90000a0000000000f90001ff93cfb4ec1a3806af0f7d71630c937ae888904ea344e0a21c"
            "692083cbba44171dab36951ce00a22288e993ed2b391e8f2b057bee990217b5b40e73abe7dc8a1cfc0d23ed399f99b80"
            "126e4c93ff0fc68a5f75ebdf922ab58e3815789e8f26c967e6119a7b38b698575ba88c0c1d2698e914b27dac3f2d77d5"
            "fb57fcef15444bb195021774d5167da9eef0a0d095c3abccf219c5455b5b5dc05d1d3031b9faee1589996498a318b372"
            "0ea70b065368fca35ae5ba3a7f0c4f9ee34c53a90504ff3368e2580984a9c393ec7422c3b92da76b72597cee248487a8"
            "66d9eda1a26c62867f6af426dcea0ebbf20793bfffd9";

        // deep_20bit_16x16: 715 bytes
        const char *const deep_20bit_16x16 =
            "ff4fff510029000000000010000000100000000000000000000000100000001000000000000000000001130101ff5200"
            "0c00000001000104040001ff5c000740a0a8a8b0ff640025000143726561746564206279204f70656e4a504547207665"
            "7273696f6e20322e352e33ff90000a00000000025e0001ff93dff955283a951813b4fb233e82110fdb7840f2ea5056af"
            "d80bcfb46eb9694008795da3b09f3de8bdf331ff709b4491c43133ce68e0ff66c67abd499b55484cbfdf3cb12099c4f1"
            "157ebe9826ebaa7f01ccade99c9da077cf9692a93b32a12c5b13aae785c814c0d40cf8dd9b63bed3ba5571d02123ac1d"
            "20cc81af510704d95b0f08c81b15749dcd4d6a9721c8209cb53561ed7bce113764dead1e136000daf640e1409e47489b"
            "8843cffcaa567fe552d1ff4a9c00071fae2958a0b624c1389d6353bc59f1d276cddfea892927c520132b06b9ca68043d"
            "3afca4032fb15aae427ab1162aaabd23dad6e61ae7340f9ad6c35ce681f35a3b7177c5c43bf5f6e2ef8b8477ebedc5df"
            "16fcefdcc35ce6ba343b986b9cd746875f6e2ef8b7ae3d6c35ce6b5cf35b0d739ad73cd6c35ce6b5cf3476e2ef88e2ed"
            "5c8c35ce9f96ec7aa1ce9415a3954394330cef618fd7f7f5ab95e866593224ac04a138d6d1600568c6556ee204344e9b"
            "32c295e8130d82df28c7a785914cafeaac5eecc7c99f025f80f658869cf812fd14387437baf3bc232d062e4cf812fc50"
            "498869cf812fca24b8869cf812fcd94ce86f75e77895aa18b933e04beef5bc434e7c097dd2b78869cf812fb6b6f10d39"
            "f025f6c0e621bdd79de04ca862e4cc45667f7bee8cfa20a7de1a36b3d9017862a8d552b8bc66624294036898013b1405"
            "f3989a8097fbc8f01b3c03f24b3d7c07e4f5b5f01f8bd3a7c07e58c38a316810d1f01f7e2b8f80fca08628c5a241cf80"
            "fb6ddcf80fb2d5ef80faeee0f80fadf66f80fcad9d68c5a8408518aca43eea81d449be9248baba1983ffd9";

        // subsampled_16x16_8x8: 470 bytes
        const char *const subsampled_16x16_8x8 =
            "ff4fff51002c000000000010000000100000000000000000000000100000001000000000000000000002070101070202"
            "ff52000c00000001000104040001ff5c00074040484850ff640025000143726561746564206279204f70656e4a504547"
            "2076657273696f6e20322e352e33ff90000a0000000001660001ff93df82103a951813b4fb233e82110fdb7840f2ea50"
            "55b961946c2d1b443119f5805b334cbfc702eb974fac53ce4987b70a4220fee9241a9203c802c8ea953a5153124a5748"
            "5bcfb44c0ed81fae37b56123d135dfb0f3820b4398ff7fcfc11a7e07f3f33c06bb043cb63768541402323393c3d68930"
            "9aed26f323a8adba2c54cc6027dc735818d509744d9c291df3455ea82b6835d7ee77f99464a81b9d2bf7537e8754fd19"
            "7d0c0d4e8d618fd7f7f5ab95e86658f3778274ae12c770efcbc659b2500d9f9be9455455577c537cae938e8e74576a94"
            "b0c8ea5ae77a3ddf9bd92cd182d7bd94c457fa3f3db1f5460447dfa7fc4e660c8aaa389564b3419860b31465511a95d2"
            "baceb088434baf1bea669043c3b5af5d41e1d96c4163270b1596a50110a4ff7fcfc0423ed0f8fc03000002608e53e73a"
            "fa80534ef1eea0b59714950e66771475506e5d9ea50c9fbb1c9f3d29b301636ddc2489dfffd9";
    } // namespace hex

    namespace fs = std::filesystem;

    std::vector<uint8_t> from_hex(const char *text)
    {
        std::vector<uint8_t> out;
        const std::string s = text;
        for (size_t i = 0; i + 1 < s.size(); i += 2)
            out.push_back((uint8_t)std::stoi(s.substr(i, 2), nullptr, 16));
        return out;
    }

    // The sample values of the codestreams above, k counting the samples of a component
    int pattern_value(size_t k, int prec) { return (int)(((uint32_t)k * 2654435761u) >> (32 - prec)); }

    // A folder of the test's own, removed afterwards
    struct TestDir
    {
        fs::path dir;

        TestDir() : dir(fs::temp_directory_path() / ("satdump-j2k-test-" + std::to_string(::getpid())))
        {
            fs::remove_all(dir);
            fs::create_directories(dir);
        }

        ~TestDir()
        {
            std::error_code ec;
            fs::remove_all(dir, ec);
        }

        std::string file(const std::string &name) const { return (dir / name).string(); }
    };

    void write_file(const std::string &path, const uint8_t *data, size_t size)
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f.write((const char *)data, size);
    }

    std::vector<uint8_t> read_file(const std::string &path)
    {
        std::ifstream f(path, std::ios::binary);
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }

    // A picture with values over the whole range, 0 and the largest value included
    Image make_picture(int depth, size_t w, size_t h, int channels)
    {
        Image img(depth, w, h, channels);
        const int maxval = (1 << depth) - 1;
        for (int c = 0; c < channels; c++)
            for (size_t y = 0; y < h; y++)
                for (size_t x = 0; x < w; x++)
                    img.set(c, x, y, (int)((x * 31 + y * 17 + c * 101 + (x * y) % 7 * 4099) % (maxval + 1)));
        img.set(0, 0, 0);
        img.set(channels - 1, w * h - 1, maxval);
        return img;
    }

    // Whether two pictures have the same depth, size, channels and pixels
    bool same_picture(Image &a, Image &b)
    {
        if (a.depth() != b.depth() || a.width() != b.width() || a.height() != b.height() || a.channels() != b.channels())
            return false;
        for (size_t i = 0; i < a.size(); i++)
            if (a.get(i) != b.get(i))
                return false;
        return true;
    }

    // Save to a .j2k file and load it again
    Image round_trip(Image &img, const TestDir &dir)
    {
        const std::string path = dir.file("round_trip.j2k");
        satdump::image::save_j2k(img, path);
        Image back;
        satdump::image::load_j2k(back, path);
        return back;
    }
} // namespace

TEST_CASE("J2K: saving and loading gives back the same pixels (8 and 16 bits, 1 and 3 channels, odd sizes)")
{
    TestDir dir;
    const size_t sizes[][2] = {{16, 16}, {17, 19}, {33, 17}, {41, 23}};
    for (int depth : {8, 16})
        for (int channels : {1, 3})
            for (auto &size : sizes)
            {
                CAPTURE(depth);
                CAPTURE(channels);
                CAPTURE(size[0]);
                CAPTURE(size[1]);
                Image img = make_picture(depth, size[0], size[1], channels);
                Image back = round_trip(img, dir);
                CHECK(same_picture(img, back));
            }
}

TEST_CASE("J2K: pictures under 16 pixels wide or high are saved (they could not be before)")
{
    TestDir dir;
    const size_t sizes[][2] = {{1, 1}, {5, 3}, {15, 40}, {40, 15}, {7, 7}};
    for (int depth : {8, 16})
        for (auto &size : sizes)
        {
            CAPTURE(depth);
            CAPTURE(size[0]);
            CAPTURE(size[1]);
            Image img = make_picture(depth, size[0], size[1], depth == 8 ? 1 : 3);
            Image back = round_trip(img, dir);
            CHECK(same_picture(img, back));
        }
}

TEST_CASE("J2K: pictures with 2, 4 and 5 channels are saved (2 crashed, 5 wrote past the end of an array)")
{
    TestDir dir;
    for (int channels : {2, 4, 5})
    {
        CAPTURE(channels);
        Image img = make_picture(8, 20, 18, channels);
        Image back = round_trip(img, dir);
        CHECK(same_picture(img, back));
    }
}

TEST_CASE("J2K: a cut-off .j2k file gives an error and no picture, or the picture with a warning")
{
    TestDir dir;
    for (int channels : {1, 3})
    {
        Image img = make_picture(channels == 1 ? 8 : 16, 40, 24, channels);
        const std::string full = dir.file("full.j2k");
        satdump::image::save_j2k(img, full);
        const std::vector<uint8_t> data = read_file(full);
        REQUIRE(data.size() > 100);

        for (size_t cut = 0; cut < data.size(); cut += (cut < 200 ? 1 : 7))
        {
            CAPTURE(channels);
            CAPTURE(cut);
            const std::string path = dir.file("cut.j2k");
            write_file(path, data.data(), cut);
            test_log::LogCapture log;
            Image back;
            satdump::image::load_j2k(back, path);
            if (back.size() == 0)
            {
                CHECK(log.has(slog::LOG_ERROR, "Could not load J2K image"));
            }
            else
            {
                CHECK(back.width() == 40);
                CHECK(back.height() == 24);
                CHECK(back.channels() == channels);
                CHECK(log.has(slog::LOG_WARN, "loaded with problems"));
            }
        }
    }
}

TEST_CASE("J2K: a garbage or damaged .j2k file does not crash, and no picture comes with an error")
{
    TestDir dir;
    Image img = make_picture(8, 40, 24, 1);
    const std::string full = dir.file("full.j2k");
    satdump::image::save_j2k(img, full);
    const std::vector<uint8_t> good = read_file(full);
    REQUIRE(good.size() > 100);

    uint32_t s = 1;
    auto next = [&s]()
    {
        s = s * 1664525u + 1013904223u;
        return (uint8_t)(s >> 24);
    };

    std::vector<std::vector<uint8_t>> inputs;
    std::vector<uint8_t> noise(1000);
    for (auto &b : noise)
        b = next();
    inputs.push_back(noise);
    for (size_t keep : {2, 4, 50, 100})
    {
        // The start of a good file, then noise
        std::vector<uint8_t> v(good.begin(), good.begin() + keep);
        for (int i = 0; i < 600; i++)
            v.push_back(next());
        inputs.push_back(v);
    }
    for (size_t pos = 0; pos < good.size(); pos += 5)
    {
        std::vector<uint8_t> v = good;
        v[pos] ^= next() | 1;
        inputs.push_back(v);
    }

    for (size_t i = 0; i < inputs.size(); i++)
    {
        CAPTURE(i);
        const std::string path = dir.file("bad.j2k");
        write_file(path, inputs[i].data(), inputs[i].size());
        test_log::LogCapture log;
        Image back;
        satdump::image::load_j2k(back, path);
        if (back.size() == 0)
            CHECK(log.has(slog::LOG_ERROR, "Could not load J2K image"));
    }
}

TEST_CASE("J2K: a picture that does not start at 0,0 loads at its real size (more was read before)")
{
    TestDir dir;
    std::vector<uint8_t> data = from_hex(hex::offset_8bit_20x12_at_3_2);
    const std::string path = dir.file("offset.j2k");
    write_file(path, data.data(), data.size());

    Image back;
    satdump::image::load_j2k(back, path);
    REQUIRE(back.width() == 20);
    REQUIRE(back.height() == 12);
    CHECK(back.depth() == 8);
    bool same = true;
    for (size_t k = 0; k < 20 * 12; k++)
        same = same && back.get(k) == pattern_value(k, 8);
    CHECK(same);

    // From memory, the values as they are in a 16-bit picture
    Image mem = satdump::image::decompress_j2k_openjp2(data.data(), (int)data.size());
    REQUIRE(mem.width() == 20);
    REQUIRE(mem.height() == 12);
    CHECK(mem.depth() == 16);
    same = true;
    for (size_t k = 0; k < 20 * 12; k++)
        same = same && mem.get(k) == pattern_value(k, 8);
    CHECK(same);
}

TEST_CASE("J2K: a 20-bit or subsampled .j2k file is refused with an error (was undefined or read too much)")
{
    TestDir dir;
    for (const char *h : {hex::deep_20bit_16x16, hex::subsampled_16x16_8x8})
    {
        const std::vector<uint8_t> data = from_hex(h);
        const std::string path = dir.file("odd.j2k");
        write_file(path, data.data(), data.size());
        test_log::LogCapture log;
        Image back;
        satdump::image::load_j2k(back, path);
        CHECK(back.size() == 0);
        CHECK(log.has(slog::LOG_ERROR, "Could not load J2K image"));
    }
}

TEST_CASE("J2K from memory: gives the first component's values as they are, in a 16-bit picture")
{
    TestDir dir;
    for (int depth : {8, 16})
    {
        CAPTURE(depth);
        Image img = make_picture(depth, 33, 17, 1);
        const std::string path = dir.file("mem.j2k");
        satdump::image::save_j2k(img, path);
        std::vector<uint8_t> data = read_file(path);
        Image mem = satdump::image::decompress_j2k_openjp2(data.data(), (int)data.size());
        REQUIRE(mem.width() == 33);
        REQUIRE(mem.height() == 17);
        CHECK(mem.depth() == 16);
        bool same = true;
        for (size_t i = 0; i < 33 * 17; i++)
            same = same && mem.get(i) == img.get(i);
        CHECK(same);
    }

    // The subsampled codestream: its first component is whole, and is what SatDump uses
    std::vector<uint8_t> sub = from_hex(hex::subsampled_16x16_8x8);
    Image mem = satdump::image::decompress_j2k_openjp2(sub.data(), (int)sub.size());
    REQUIRE(mem.width() == 16);
    REQUIRE(mem.height() == 16);
    bool same = true;
    for (size_t k = 0; k < 16 * 16; k++)
        same = same && mem.get(k) == pattern_value(k, 8);
    CHECK(same);
}

TEST_CASE("J2K from memory: no data, or a length under 0, gives a warning and no picture")
{
    uint8_t data[16] = {0xFF, 0x4F, 0xFF, 0x51};
    for (int length : {0, -1, -1000})
    {
        CAPTURE(length);
        test_log::LogCapture log;
        Image img = satdump::image::decompress_j2k_openjp2(data, length);
        CHECK(img.size() == 0);
        CHECK(log.has(slog::LOG_WARN, "J2K: no data to decompress"));
    }

    // A null pointer with a length (it was read from)
    test_log::LogCapture log;
    Image img = satdump::image::decompress_j2k_openjp2(nullptr, 16);
    CHECK(img.size() == 0);
    CHECK(log.has(slog::LOG_WARN, "J2K: no data to decompress"));
}

TEST_CASE("J2K from memory: cut-off and damaged data give a warning, never a crash")
{
    const std::vector<uint8_t> good = from_hex(hex::offset_8bit_20x12_at_3_2);
    for (size_t cut = 1; cut <= good.size(); cut++)
    {
        CAPTURE(cut);
        // A copy of exactly cut bytes, so a sanitizer build sees any read past the end
        std::vector<uint8_t> data(good.begin(), good.begin() + cut);
        test_log::LogCapture log;
        Image img = satdump::image::decompress_j2k_openjp2(data.data(), (int)data.size());
        if (img.size() == 0)
        {
            CHECK(log.has(slog::LOG_WARN, "J2K: could not decompress"));
        }
        else
        {
            CHECK(img.width() == 20);
            CHECK(img.height() == 12);
            if (cut < good.size())
                CHECK(log.has(slog::LOG_WARN, "decompressed with problems"));
        }
    }
    for (size_t pos = 0; pos < good.size(); pos += 3)
    {
        CAPTURE(pos);
        std::vector<uint8_t> data = good;
        data[pos] ^= 0x5A;
        test_log::LogCapture log;
        Image img = satdump::image::decompress_j2k_openjp2(data.data(), (int)data.size());
        if (img.size() == 0)
            CHECK(log.has(slog::LOG_WARN, "J2K: could not decompress"));
    }
}
