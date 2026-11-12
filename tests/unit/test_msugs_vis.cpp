// ELEKTRO-L MSU-GS visible channel line reader (elektro_arktika::msugs::MSUVISReader). A line
// frame is 15210 bytes (121680 bits, as the module's deframer cuts them): the line counter in
// bytes 8-9, a 40-bit time stamp in 1/256 s in bytes 10-14, and 10-bit pixels from byte 190.
#include "elektro_arktika/instruments/msugs/msu_vis_reader.h"
#include "doctest.h"
#include <cstdint>
#include <vector>

namespace
{
    const size_t FRAME = 121680 / 8;

    std::vector<uint8_t> line(int counter, const uint8_t time[5])
    {
        std::vector<uint8_t> f(FRAME);
        for (size_t i = 0; i < FRAME; i++)
            f[i] = uint8_t(i * 7 + 3);
        f[8] = uint8_t(counter >> 8);
        f[9] = uint8_t(counter & 0xFF);
        for (int i = 0; i < 5; i++)
            f[10 + i] = time[i];
        return f;
    }

    // Value n of the line's 10-bit values: 4 values in each 5 bytes, from byte 190
    int value10(const std::vector<uint8_t> &f, int n)
    {
        const uint8_t *b = &f[190 + (n / 4) * 5];
        switch (n % 4)
        {
        case 0:
            return b[0] << 2 | b[1] >> 6;
        case 1:
            return (b[1] & 0x3F) << 4 | b[2] >> 4;
        case 2:
            return (b[2] & 0x0F) << 6 | b[3] >> 2;
        default:
            return (b[3] & 0x03) << 8 | b[4];
        }
    }

    // The time the reader should give for these 5 bytes
    double expected_time(const uint8_t t[5])
    {
        uint64_t v = (uint64_t)t[0] << 32 | (uint64_t)t[1] << 24 | (uint64_t)t[2] << 16 | (uint64_t)t[3] << 8 | (uint64_t)t[4];
        return v / 256.0 + 1735204808.2837029 - (1800 + 88);
    }
} // namespace

TEST_CASE("MSU-GS VIS: the line time stamp is the 40-bit count in bytes 10-14 (was shifted past 32 bits and sign-extended)")
{
    elektro_arktika::msugs::MSUVISReader reader;

    // Top byte set (lost before) and byte 11 >= 0x80 (made the value negative before)
    const uint8_t late[5] = {0x01, 0x9A, 0x12, 0x34, 0x56};
    auto f1 = line(5, late);
    reader.pushFrame(f1.data(), false);
    CHECK(reader.timestamps[5] == expected_time(late));
    CHECK(reader.timestamps[5] > 1735204808.0);

    // Small counts were already right and stay so
    const uint8_t early[5] = {0x00, 0x12, 0x34, 0x56, 0x78};
    auto f2 = line(6, early);
    reader.pushFrame(f2.data(), false);
    CHECK(reader.timestamps[6] == expected_time(early));

    CHECK(reader.frames == 2);
}

TEST_CASE("MSU-GS VIS: a line frame gives its 2 x 6004 pixels, reading nothing past the end of the frame")
{
    // The frame is exactly as long as the deframer makes it, so a sanitizer build stops on any
    // read past its end (the old loop read 35 bytes too far)
    elektro_arktika::msugs::MSUVISReader reader;
    const uint8_t t[5] = {0, 0, 0, 1, 0};
    auto f = line(7, t);
    const std::vector<uint8_t> sent = f; // pushFrame() reorders bytes 15200-15208 in place
    reader.pushFrame(f.data(), false);

    int wrong = 0;
    {
        image::Image img = reader.getImage1();
        REQUIRE(img.width() == 6004);
        for (int x = 0; x < 6004; x++)
            wrong += img.get(7 * 6004 + x) != value10(sent, 2 * x) << 6;
    }
    {
        image::Image img = reader.getImage2();
        for (int x = 0; x < 6004; x++)
            wrong += img.get(7 * 6004 + x) != value10(sent, 2 * x + 1) << 6;
    }
    CHECK(wrong == 0);
}
