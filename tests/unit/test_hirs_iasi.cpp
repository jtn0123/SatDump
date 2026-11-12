#include "common/calibration.h"
#include "doctest.h"
#include "metop/instruments/iasi/iasi_imaging_reader.h"
#include "noaa/instruments/hirs/hirs_reader.h"
#include <cfenv>
#include <cstdint>
#include <vector>

namespace
{
    // Where HIRSReader takes its 36 bytes from in a 104-byte TIP minor frame, and the channel
    // each of the 20 words goes to (copied from hirs_reader.h)
    const int HIRS_POSITIONS[36] = {16, 17, 22, 23, 26, 27, 30, 31, 34, 35, 38, 39, 42, 43, 54, 55, 58, 59,
                                    62, 63, 66, 67, 70, 71, 74, 75, 78, 79, 82, 83, 84, 85, 88, 89, 92, 93};
    const int HIRS_CHANNELS[20] = {0, 16, 1, 2, 12, 3, 17, 10, 18, 6, 7, 19, 9, 13, 5, 4, 14, 11, 15, 8};

    // Encoder positions: the reader counts 68 as a space view and 156 as a blackbody view.
    // 10 is an Earth view, 100 a position whose words are stored as they are.
    constexpr int SPACE = 68, BLACKBODY = 156, EARTH = 10, RAW = 100;

    // A word with bit 12 set is read as 4095 + its low 12 bits. Averaged and shifted right by 3,
    // these give a space count of 4195 >> 3 = 524 and a blackbody count of 6095 >> 3 = 761.
    constexpr uint16_t SPACE_WORD = 0x1000 | 100;
    constexpr uint16_t BB_WORD = 0x1000 | 2000;

    // One TIP minor frame carrying HIRS element `elnum` with these 20 13-bit words. The words
    // start 2 bits into the 4th HIRS byte, after the element number's last bit and a sync bit.
    std::vector<uint8_t> hirs_frame(int elnum, int encoder, const uint16_t words[20])
    {
        uint8_t hirs[36] = {};
        hirs[0] = encoder;
        hirs[2] = (elnum >> 1) & 0x1F;
        hirs[3] = (elnum & 1) << 7;
        for (int w = 0; w < 20; w++)
            for (int b = 0; b < 13; b++)
            {
                int bit = 26 + w * 13 + b;
                if ((words[w] >> (12 - b)) & 1)
                    hirs[bit / 8] |= 0x80 >> (bit % 8);
            }
        hirs[35] |= 0b10; // scan data present

        std::vector<uint8_t> frame(104, 0);
        frame[5] = 1; // minor frame 1, which has no time code
        for (int k = 0; k < 36; k++)
            frame[HIRS_POSITIONS[k]] = hirs[k];
        return frame;
    }

    // A pass fed to HIRSReader frame by frame
    struct HirsPass
    {
        noaa::hirs::HIRSReader r{2025};

        // Elements first to last of a scan line, all 20 words the same. Element 55 ends a line.
        void elements(int first, int last, int encoder, uint16_t word)
        {
            uint16_t words[20];
            for (uint16_t &w : words)
                w = word;
            for (int e = first; e <= last; e++)
            {
                std::vector<uint8_t> frame = hirs_frame(e, encoder, words);
                r.work(frame.data());
            }
        }

        // Elements 58 and 59, sent after a line: the readings of the five blackbody thermometers (PRTs)
        void prt(uint16_t reading) { elements(58, 59, 0, reading); }

        double coef(const char *name, int channel, int line)
        {
            return r.calib_out["vars"]["perLine_perChannel"][channel][line][name].get<double>();
        }
    };

    // Every PRT reads 300 K - 0.01 K per count; b = 0 and c = 1 keep that temperature
    nlohmann::json hirs_coefs()
    {
        nlohmann::json c;
        for (int p = 0; p < 5; p++)
            c["PRT_poly"][p] = {300.0, 0.01, 0.0, 0.0, 0.0, 0.0};
        for (int ch = 0; ch < 19; ch++)
        {
            c["b"][ch] = 0.0;
            c["c"][ch] = 1.0;
            c["wavenumber"][ch] = 700.0;
        }
        c["visChannel"] = {{"a0", 0.0}, {"a1", 1.0}};
        return c;
    }
} // namespace

TEST_CASE("HIRS: the 20 words are unpacked without reading past the 36 HIRS bytes")
{
    // shift_array_left() reads one byte past the 33 it shifts: that was HIRS_data[36], one past
    // the end of the array. The sanitizer build catches the read; the words must stay the same.
    HirsPass pass;
    for (int e = 0; e < 56; e++)
    {
        uint16_t words[20];
        for (int i = 0; i < 20; i++)
            words[i] = e == 0 ? 0x1FFF : (i * 409 + e * 53 + 1) & 0x1FFF; // element 0: every bit set
        std::vector<uint8_t> frame = hirs_frame(e, RAW, words);
        pass.r.work(frame.data());
    }
    REQUIRE(pass.r.line == 1);

    for (int i = 0; i < 20; i++)
    {
        CAPTURE(i);
        satdump::image::Image img = pass.r.getChannel(HIRS_CHANNELS[i]);
        REQUIRE(img.width() == 56);
        REQUIRE(img.height() == 1);
        for (int e = 0; e < 56; e++)
        {
            CAPTURE(e);
            CHECK(img.get(55 - e) == (e == 0 ? 0x1FFF : (i * 409 + e * 53 + 1) & 0x1FFF));
        }
    }
}

TEST_CASE("HIRS: averaging samples that are all 0 gives 0, without a NaN")
{
    // The mean was 0 / 0, and that NaN was converted to int (undefined behaviour). Both raise
    // the floating-point "invalid" flag, which a normal build can test.
    uint16_t zeros[5] = {0, 0, 0, 0, 0};
    std::feclearexcept(FE_ALL_EXCEPT);
    uint16_t avg = noaa::hirs::calib_sequence::calc_avg(zeros, 5);
    bool invalid = std::fetestexcept(FE_INVALID) != 0;
    CHECK(avg == 0);
    CHECK_FALSE(invalid);

    // Zeros are left out of the average; other samples are averaged as before
    uint16_t some[5] = {0, 800, 800, 800, 0};
    CHECK(noaa::hirs::calib_sequence::calc_avg(some, 5) == 800);
    uint16_t spread[5] = {100, 200, 300, 400, 500}; // mean 300, sigma 141: 200, 300 and 400 count
    CHECK(noaa::hirs::calib_sequence::calc_avg(spread, 5) == 300);
}

TEST_CASE("HIRS calibration: a sequence at the end of the pass uses the last PRT reading")
{
    // The temperature of a sequence on line P comes from the PRT readings of lines P - 1, P and
    // P + 1. Here the pass ends on line 1, right after the sequence, so line 2 has no reading:
    // index 2 of a 2-element vector was read (the sanitizer build stops there; a normal build
    // got a temperature from stray bytes). Now the last reading stands in for it.
    HirsPass pass;
    pass.elements(0, 55, SPACE, SPACE_WORD); // line 0: space view
    pass.prt(1000);
    pass.elements(0, 55, BLACKBODY, BB_WORD); // line 1: blackbody view, the sequence is complete
    pass.prt(1000);
    REQUIRE(pass.r.line == 2);

    pass.r.calibrate(hirs_coefs(), false);

    const double rad = temperature_to_radiance(300.0 - 0.01 * 1000, 700.0);
    const double a1 = rad / (761 - 524);
    for (int ch = 0; ch < 19; ch++)
        for (int line = 0; line < 2; line++)
        {
            CAPTURE(ch);
            CAPTURE(line);
            CHECK(pass.coef("a1", ch, line) == doctest::Approx(a1).epsilon(1e-9));
            CHECK(pass.coef("a0", ch, line) == doctest::Approx(-a1 * 524).epsilon(1e-9));
        }
}

TEST_CASE("HIRS calibration: a sequence on the first line does not read line -1")
{
    // The space line's element 55 was lost, so the space and blackbody views both fall on line 0
    // and the sequence completes there. Its temperature asked for the PRT reading of line -1.
    HirsPass pass;
    pass.elements(0, 54, SPACE, SPACE_WORD);
    pass.elements(0, 55, BLACKBODY, BB_WORD);
    pass.prt(1000);
    pass.elements(0, 55, EARTH, SPACE_WORD); // line 1: Earth view
    pass.prt(1000);
    REQUIRE(pass.r.line == 2);

    pass.r.calibrate(hirs_coefs(), false);

    // The space step cleared the line before the blackbody average was taken, so this sequence
    // has no blackbody count and no line is calibrated, before the fix as after it. Before the
    // fix the sanitizer build stopped on the read of line -1 (index check, or AddressSanitizer);
    // a clang build stopped earlier, on the NaN of that all-zero blackbody average.
    for (int ch = 0; ch < 19; ch++)
        for (int line = 0; line < 2; line++)
        {
            CAPTURE(ch);
            CAPTURE(line);
            CHECK(pass.coef("a0", ch, line) == -999.99);
            CHECK(pass.coef("a1", ch, line) == -999.99);
        }
}

TEST_CASE("HIRS calibration: without PRT readings no line is calibrated")
{
    // A complete sequence, but every element 58 and 59 frame was lost: the empty PRT vectors
    // were read through a null pointer (a crash)
    HirsPass pass;
    pass.elements(0, 55, SPACE, SPACE_WORD);
    pass.elements(0, 55, BLACKBODY, BB_WORD);
    REQUIRE(pass.r.line == 2);

    pass.r.calibrate(hirs_coefs(), false);

    for (int ch = 0; ch < 19; ch++)
        for (int line = 0; line < 2; line++)
        {
            CAPTURE(ch);
            CAPTURE(line);
            CHECK(pass.coef("a0", ch, line) == -999.99);
            CHECK(pass.coef("a1", ch, line) == -999.99);
        }
}

TEST_CASE("IASI imaging: a pixel whose warm and cold views read the same is 0 (no data)")
{
    // One line of 36 frames of 64 x 64 pixels: frames 0-1 view cold space, 3-4 the warm
    // blackbody and 6-35 the Earth. Frames 0-5 are cropped off the picture.
    metop::iasi::IASIIMGReader r;
    r.lines = 1;
    REQUIRE(r.ir_channel.size() >= size_t(64 * 36 * 64));
    auto at = [&](int frame, int x, int y) -> uint16_t & { return r.ir_channel[y * 36 * 64 + frame * 64 + x]; };
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++)
            for (int f = 0; f < 36; f++)
                at(f, x, y) = f < 2 ? 1000 : (f == 3 || f == 4) ? 3000 : 2000;

    // Three pixels on row 7 whose warm views read 1000 like their cold views, with an Earth view
    // above (x 5), equal to (x 6) and below (x 7) that. They gave +inf (shown as 65535), NaN
    // (converted to int: undefined behaviour) and -inf (shown as 0).
    const int earth[3] = {2000, 1000, 500};
    for (int k = 0; k < 3; k++)
    {
        at(3, 5 + k, 7) = at(4, 5 + k, 7) = 1000;
        for (int f = 6; f < 36; f++)
            at(f, 5 + k, 7) = earth[k];
    }

    satdump::image::Image img = r.getIRChannel();
    REQUIRE(img.width() == 30 * 64);
    REQUIRE(img.height() == 64);
    auto out = [&](int frame, int x, int y) { return img.get(y * 30 * 64 + (frame - 6) * 64 + x); };

    // Ordinary pixels: (2000 - 1000) / (3000 - 1000), scaled to pixel (31, 0), is 2000 again
    CHECK(out(6, 0, 0) == 2000);
    CHECK(out(20, 4, 7) == 2000);
    CHECK(out(35, 63, 63) == 2000);

    for (int k = 0; k < 3; k++)
        for (int f = 6; f < 36; f++)
        {
            CAPTURE(k);
            CAPTURE(f);
            CHECK(out(f, 5 + k, 7) == 0);
        }
}
