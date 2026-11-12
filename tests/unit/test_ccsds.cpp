#include "common/ccsds/ccsds.h"
#include "common/ccsds/ccsds_time.h"
#include "doctest.h"
#include <cstdint>
#include <cstring>
#include <vector>

namespace
{
    ccsds::CCSDSPacket make_packet(const uint8_t header[6], std::vector<uint8_t> payload)
    {
        ccsds::CCSDSPacket pkt;
        std::memcpy(pkt.header.raw, header, 6);
        pkt.payload = std::move(payload);
        return pkt;
    }

    const uint8_t ZERO_HEADER[6] = {0, 0, 0, 0, 0, 0};
} // namespace

TEST_CASE("CCITT CRC: the standard check value for \"123456789\" is 0x29B1")
{
    // The check covers the 6 header bytes and the payload, minus the 2 CRC bytes at the end
    const uint8_t header[6] = {'1', '2', '3', '4', '5', '6'};
    ccsds::CCSDSPacket pkt = make_packet(header, {'7', '8', '9', 0x29, 0xB1});
    CHECK(ccsds::crcCheckCCITT(pkt));

    pkt.payload[1] ^= 0x01;
    CHECK_FALSE(ccsds::crcCheckCCITT(pkt));
}

TEST_CASE("Vertical parity: XOR of all 16-bit words equals the last word")
{
    const uint8_t header[6] = {0x08, 0x01, 0xC0, 0x02, 0x00, 0x0B};
    std::vector<uint8_t> payload = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    uint16_t x = 0;
    for (int i = 0; i < 6; i += 2)
        x ^= header[i] << 8 | header[i + 1];
    for (size_t i = 0; i < payload.size(); i += 2)
        x ^= payload[i] << 8 | payload[i + 1];
    payload.push_back(x >> 8);
    payload.push_back(x & 0xFF);

    ccsds::CCSDSPacket pkt = make_packet(header, payload);
    CHECK(ccsds::crcCheckVerticalParity(pkt));
    pkt.payload[4] ^= 0x80;
    CHECK_FALSE(ccsds::crcCheckVerticalParity(pkt));
}

TEST_CASE("CRC checks reject payloads too short to hold a CRC (no reads outside the packet)")
{
    // A corrupt length field can give a 0- or 1-byte payload; the sanitizer build catches any stray read
    for (size_t n : {0, 1, 3})
    {
        CAPTURE(n);
        ccsds::CCSDSPacket pkt = make_packet(ZERO_HEADER, std::vector<uint8_t>(n, 0));
        if (n < 2)
        {
            CHECK_FALSE(ccsds::crcCheckCCITT(pkt));
            CHECK_FALSE(ccsds::crcCheckVerticalParity(pkt));
        }
        CHECK_FALSE(ccsds::crcCheckHLDC32(pkt));
    }
}

TEST_CASE("CCSDS day-segmented time")
{
    // day 9445 after the 2000-01-01 epoch (offset 10957 days from 1970) = 2025-11-10, at 12:34:56.789
    const uint32_t ms = (12 * 3600 + 34 * 60 + 56) * 1000 + 789;
    std::vector<uint8_t> payload = {0x24, 0xE5, uint8_t(ms >> 24), uint8_t(ms >> 16), uint8_t(ms >> 8), uint8_t(ms), 0x01, 0xF4};
    ccsds::CCSDSPacket pkt = make_packet(ZERO_HEADER, payload);

    CHECK(ccsds::parseCCSDSTime(pkt, 10957) == time_t(1762778096));
    CHECK(ccsds::parseCCSDSTimeFull(pkt, 10957) == doctest::Approx(1762778096.789 + 0.0005).epsilon(1e-12));
    CHECK(ccsds::parseCCSDSTimeFullRaw(payload.data(), 10957) == doctest::Approx(1762778096.7895).epsilon(1e-12));
}

TEST_CASE("CCSDS time does not overflow for large day counts (corrupt data, or dates after 2038)")
{
    // 0xFFFF days after the 2000 epoch, and a milliseconds field with its top bit set
    ccsds::CCSDSPacket pkt = make_packet(ZERO_HEADER, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0});
    const time_t expect = (time_t(10957) + 65535) * 86400 + time_t(0xFFFFFFFFu / 1000);
    CHECK(ccsds::parseCCSDSTime(pkt, 10957) == expect);
}

TEST_CASE("CCSDS time from a packet too short to hold one is -1 (the usual 'no time' value)")
{
    for (size_t n : {0, 3, 5})
    {
        CAPTURE(n);
        ccsds::CCSDSPacket pkt = make_packet(ZERO_HEADER, std::vector<uint8_t>(n, 0x11));
        CHECK(ccsds::parseCCSDSTime(pkt, 10957) == -1);
    }
    for (size_t n : {0, 6, 7})
    {
        CAPTURE(n);
        ccsds::CCSDSPacket pkt = make_packet(ZERO_HEADER, std::vector<uint8_t>(n, 0x11));
        CHECK(ccsds::parseCCSDSTimeFull(pkt, 10957) == -1);
    }
}
