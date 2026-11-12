#include "doctest.h"
#include "instruments/amsu/amsu_reader.h"
#include "instruments/mhs/mhs_reader.h"
#include "metop/instruments/gome/gome_reader.h"
#include "nlohmann/json.hpp"
#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace
{
    // A packet with the given APID (header from raw bytes, so every field is set) and a zeroed payload
    ccsds::CCSDSPacket make_packet(uint16_t apid, size_t size)
    {
        uint8_t hdr[6] = {uint8_t(0x08 | (apid >> 8)), uint8_t(apid & 0xFF), 0xC0, 0, 0, 0};
        ccsds::CCSDSPacket pkt;
        pkt.header = ccsds::CCSDSHeader(hdr);
        pkt.payload.assign(size, 0);
        return pkt;
    }

    // ---------------------------------------------------------------------------- AMSU

    // A MetOp AMSU packet (APID 39 = A1, 2096 bytes; APID 40 = A2, 1136 bytes): from byte 14 on,
    // the instrument frame, then 0x0001 fill words, which the reader removes
    ccsds::CCSDSPacket amsu_packet(uint16_t apid, size_t size, const std::vector<uint8_t> &frame)
    {
        ccsds::CCSDSPacket pkt = make_packet(apid, size);
        for (size_t i = 14; i + 1 < size; i += 2)
            pkt.payload[i + 1] = 0x01;
        REQUIRE(14 + frame.size() <= size);
        std::copy(frame.begin(), frame.end(), pkt.payload.begin() + 14);
        return pkt;
    }

    // A 312-byte AMSU-A2 frame. Scan position k (0-29) has channel 1 in bytes 8k+12..13 and
    // channel 2 in bytes 8k+14..15; every byte of a word differs, and no word is 0x0001
    std::vector<uint8_t> a2_frame()
    {
        std::vector<uint8_t> f(312, 0xAA);
        for (int k = 0; k < 30; k++)
        {
            f[8 * k + 12] = uint8_t(0x10 + k);
            f[8 * k + 13] = uint8_t(0x20 + k);
            f[8 * k + 14] = uint8_t(0x30 + k);
            f[8 * k + 15] = uint8_t(0x50 + k);
        }
        return f;
    }

    // ---------------------------------------------------------------------------- GOME

    // GOME sample values for readout r (0-1), detector d (0-3), pixel p (0-1023), all different
    uint16_t gome_value(int r, int d, int p) { return uint16_t((r * 4 + d + 1) * 2048 + p); }

    // A GOME packet (18732 bytes) holding gome_value() for both readouts of every detector pixel.
    // counter (0-15) is the packet's place in the line; 15 ends the line.
    ccsds::CCSDSPacket gome_packet(int counter)
    {
        ccsds::CCSDSPacket pkt = make_packet(384, 18732);
        // The reader's 16-bit words start at byte 14; word 6 is the counter
        pkt.payload[14 + 2 * 6 + 1] = uint8_t(counter);
        // From word 478 + 680: 2 readouts x 4 detectors, each one word then 1024 samples
        const size_t base = 14 + 2 * (478 + 680);
        for (int r = 0; r < 2; r++)
            for (int d = 0; d < 4; d++)
                for (int p = 0; p < 1024; p++)
                {
                    const size_t at = base + (r * 4 + d) * 2050 + 2 + 2 * p;
                    const uint16_t v = gome_value(r, d, p);
                    pkt.payload[at] = uint8_t(v >> 8);
                    pkt.payload[at + 1] = uint8_t(v & 0xFF);
                }
        return pkt;
    }

    // ---------------------------------------------------------------------------- MHS

    // A MetOp MHS packet (APID 34, 1302 bytes); the housekeeping bytes start at byte 14
    ccsds::CCSDSPacket mhs_packet()
    {
        ccsds::CCSDSPacket pkt = make_packet(34, 1302);
        pkt.payload[14 + 9] = 8;   // HK[9]: the first thermistor ("LO H1")
        pkt.payload[14 + 33] = 10; // HK[33]: the first current ("EE and SM +5V ")
        return pkt;
    }
} // namespace

TEST_CASE("AMSU-A2: channel 2 is the word in bytes 14-15 (it was built from byte 14 twice)")
{
    noaa_metop::amsu::AMSUReader r;
    ccsds::CCSDSPacket pkt = amsu_packet(40, 1136, a2_frame());
    r.work_metop(pkt);
    REQUIRE(r.linesA2 == 1);

    satdump::image::Image ch1 = r.getChannel(0);
    satdump::image::Image ch2 = r.getChannel(1);
    REQUIRE(ch2.width() == 30);
    for (int k = 0; k < 30; k++)
    {
        CAPTURE(k);
        // getChannel() mirrors the line: scan position k is pixel 29 - k
        CHECK(ch1.get(29 - k) == ((0x10 + k) << 8 | (0x20 + k))); // unchanged
        CHECK(ch2.get(29 - k) == ((0x30 + k) << 8 | (0x50 + k)));
    }
}

TEST_CASE("AMSU (MetOp): a packet left short by the 0x0001 filter is dropped, not read past its end")
{
    // A1 frames are 1240 bytes and A2 frames 312. With one data word less (a damaged packet with
    // an extra 0x0001 word), the line was built from bytes past the end of the filtered vector.
    noaa_metop::amsu::AMSUReader r;
    ccsds::CCSDSPacket a1_short = amsu_packet(39, 2096, std::vector<uint8_t>(1238, 0xAA));
    ccsds::CCSDSPacket a2_short = amsu_packet(40, 1136, std::vector<uint8_t>(310, 0xAA));
    r.work_metop(a1_short);
    r.work_metop(a2_short);
    CHECK(r.linesA1 == 0);
    CHECK(r.linesA2 == 0);

    // Whole frames are still decoded
    ccsds::CCSDSPacket a1 = amsu_packet(39, 2096, std::vector<uint8_t>(1240, 0xAA));
    ccsds::CCSDSPacket a2 = amsu_packet(40, 1136, a2_frame());
    r.work_metop(a1);
    r.work_metop(a2);
    CHECK(r.linesA1 == 1);
    CHECK(r.linesA2 == 1);
    CHECK(r.getChannel(2).get(0) == 0xAAAA); // AMSU-A1 channel 3
}

TEST_CASE("GOME: band 1 (detector 0, pixels 659-1023) is filled; it was always skipped")
{
    auto g = std::make_unique<metop::gome::GOMEReader>();
    ccsds::CCSDSPacket pkt = gome_packet(0);
    g->work(pkt);
    CHECK(g->channel_number == 4096);

    // Counter 0: readout 0 is sample 31 of the line, readout 1 is sample 30
    CHECK(g->channels[0][5][31] == gome_value(0, 0, 5)); // band 0: pixels 0-658, unchanged
    CHECK(g->channels[1][0][31] == gome_value(0, 0, 659));
    CHECK(g->channels[1][0][30] == gome_value(1, 0, 659));
    CHECK(g->channels[1][364][31] == gome_value(0, 0, 1023));
    CHECK(g->channels[1][364][30] == gome_value(1, 0, 1023));
    CHECK(g->channels[3][0][31] == gome_value(0, 1, 71)); // band 3: detector 1 from pixel 71, unchanged
    CHECK(g->channels[5][1023][30] == gome_value(1, 3, 1023));
}

TEST_CASE("GOME: getChannel() counts the channels band after band (it was one off at each band start)")
{
    auto g = std::make_unique<metop::gome::GOMEReader>();
    g->lines = 1;
    for (int b = 0; b < 6; b++)
        for (int c = 0; c < 1024; c++)
            g->channels[b][c].assign(32, uint16_t(b * 1024 + c));

    // {product channel, band, channel in the band} at the start and end of every band.
    // 3072 used to read channels[4][1024], one past the end of the array (it happened to hold channels[5][0]).
    const int cases[][3] = {{0, 0, 0}, {658, 0, 658}, {659, 1, 0}, {1023, 1, 364}, {1024, 2, 0}, {1094, 2, 70},
                            {1095, 3, 0}, {2047, 3, 952}, {2048, 4, 0}, {3071, 4, 1023}, {3072, 5, 0}, {4095, 5, 1023}};
    for (const auto &t : cases)
    {
        CAPTURE(t[0]);
        satdump::image::Image img = g->getChannel(t[0]);
        REQUIRE(img.width() == 32);
        REQUIRE(img.height() == 1);
        CHECK(img.get(0) == t[1] * 1024 + t[2]);
    }
}

TEST_CASE("GOME: product channel i is detector i / 1024, pixel i % 1024")
{
    auto g = std::make_unique<metop::gome::GOMEReader>();
    ccsds::CCSDSPacket pkt = gome_packet(15); // the last packet of a line
    g->work(pkt);
    REQUIRE(g->lines == 1);
    REQUIRE(g->channel_number == 4096);

    // Counter 15: readout 0 is sample 1 of the line, readout 1 is sample 0
    std::vector<int> wrong;
    for (int i = 0; i < g->channel_number; i++)
    {
        satdump::image::Image img = g->getChannel(i);
        if (img.width() != 32 || img.height() != 1 ||
            img.get(1) != gome_value(0, i / 1024, i % 1024) || img.get(0) != gome_value(1, i / 1024, i % 1024))
            wrong.push_back(i);
    }
    const int first_wrong = wrong.empty() ? -1 : wrong.front();
    CAPTURE(first_wrong);
    CHECK(wrong.size() == 0u);
}

TEST_CASE("MHS telemetry: a satellite without coefficients (MetOp-A) gets no temperatures or currents instead of an exception")
{
    noaa_metop::mhs::MHSReader r;
    ccsds::CCSDSPacket pkt = mhs_packet();
    r.work_metop(pkt);
    REQUIRE(r.line == 1);

    // MHS.json has no "MetOp-A" entry, so the module passes calib_coefs["MetOp-A"], which is null
    nlohmann::json calib_coefs;
    calib_coefs["MetOp-B"]["g"] = nlohmann::json::array({0.0, 0.0, 0.0, 0.0, 0.0});
    nlohmann::json tlm;
    CHECK_NOTHROW(tlm = r.dump_telemetry(calib_coefs["MetOp-A"]));
    REQUIRE(tlm.is_object());
    CHECK(tlm["misc"]["mode"].size() == 1);
    CHECK(tlm["misc"]["mode"][0].get<std::string>() == "power on");
    CHECK(tlm["switches"].is_object());
    CHECK_FALSE(tlm.contains("temperatures"));
    CHECK_FALSE(tlm.contains("currents"));
}

TEST_CASE("MHS telemetry: with coefficients, temperatures and currents are computed as before")
{
    noaa_metop::mhs::MHSReader r;
    ccsds::CCSDSPacket pkt = mhs_packet();
    r.work_metop(pkt);
    REQUIRE(r.line == 1);

    nlohmann::json coefs;
    coefs["g"] = nlohmann::json::array({1.5, 0.25, 0.0, 0.0, 0.0});
    for (int c = 0; c < 6; c++)
        coefs["current_coefs"].push_back(nlohmann::json::array({0.5, 2.0}));

    nlohmann::json tlm = r.dump_telemetry(coefs);
    CHECK(tlm["temperatures"].size() == 24);
    CHECK(tlm["currents"].size() == 6);
    CHECK(tlm["temperatures"]["LO H1"][0].get<double>() == doctest::Approx(1.5 + 0.25 * 8));
    CHECK(tlm["currents"]["EE and SM +5V "][0].get<double>() == doctest::Approx(0.5 + 2.0 * 10));
}
