#include "doctest.h"
#include "libs/bzlib/bzlib.h"
#include "metop/instruments/admin_msg/admin_msg_reader.h"
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

namespace
{
    // A MetOp admin message packet: 41 bytes before the bzip2 stream
    ccsds::CCSDSPacket admin_packet(const std::string &xml, int seq = 7)
    {
        std::vector<char> compressed(xml.size() + 1024);
        unsigned int csize = compressed.size();
        REQUIRE(BZ2_bzBuffToBuffCompress(compressed.data(), &csize, const_cast<char *>(xml.data()), xml.size(), 9, 0, 0) == BZ_OK);

        ccsds::CCSDSPacket pkt;
        pkt.header.packet_sequence_count = seq;
        pkt.payload.assign(41, 0);
        pkt.payload.insert(pkt.payload.end(), compressed.begin(), compressed.begin() + csize);
        return pkt;
    }

    std::string tle_message(const std::string &attrs, const std::string &lines)
    {
        return "<message " + attrs + "><navigation><two-line-elements>" + lines + "</two-line-elements></navigation></message>";
    }

    const std::string L1 = "<line-1>1 38771U 12049A   25313.50000000  .00000000  00000-0  00000-0 0  9990</line-1>";
    const std::string L2 = "<line-2>2 38771  98.7000 300.0000 0001000  90.0000 270.0000 14.21000000 00000</line-2>";

    struct Reader
    {
        std::filesystem::path dir;
        metop::admin_msg::AdminMsgReader r;
        Reader()
        {
            dir = std::filesystem::temp_directory_path() / ("satdump-admin-test-" + std::to_string(::getpid()));
            std::filesystem::create_directories(dir);
            r.directory = dir.string();
        }
        ~Reader() { std::filesystem::remove_all(dir); }
    };
} // namespace

TEST_CASE("Admin message: TLEs are read from a valid message")
{
    Reader t;
    const std::string xml = "<multi-mission-administrative-message>" +
                            tle_message("satellite=\"M01\" satellite-number=\"38771\"", L1 + L2) +
                            tle_message("satellite=\"M03\" satellite-number=\"43689\"", L1 + L2) +
                            "</multi-mission-administrative-message>";
    ccsds::CCSDSPacket pkt = admin_packet(xml);
    t.r.work(pkt);

    CHECK(t.r.count == 1);
    REQUIRE(t.r.tles.size() == 2);
    CHECK(t.r.tles[0].norad == 38771);
    CHECK(t.r.tles[0].name == "M01");
    CHECK(t.r.tles[0].line1.rfind("1 38771U", 0) == 0);
    CHECK(t.r.tles[0].line2.rfind("2 38771 ", 0) == 0);
    CHECK(t.r.tles[1].norad == 43689);
    CHECK(std::filesystem::exists(t.dir / "7.xml"));
}

TEST_CASE("Admin message: packets too short to hold a message are skipped")
{
    Reader t;
    for (size_t n : {0, 1, 40, 41})
    {
        CAPTURE(n);
        ccsds::CCSDSPacket pkt;
        pkt.payload.assign(n, 0);
        t.r.work(pkt);
    }
    CHECK(t.r.count == 0);
    CHECK(t.r.tles.empty());
}

TEST_CASE("Admin message: unexpected XML never crashes and adds no incomplete TLEs")
{
    const std::vector<std::string> cases = {
        "<some-other-root/>",                                     // wrong root element
        "<multi-mission-administrative-message><message",         // malformed
        "<multi-mission-administrative-message>" +                // no attributes
            tle_message("", L1 + L2) + "</multi-mission-administrative-message>",
        "<multi-mission-administrative-message>" +                // only line 1
            tle_message("satellite=\"M01\" satellite-number=\"38771\"", L1) + "</multi-mission-administrative-message>",
        "<multi-mission-administrative-message>" +                // not a number
            tle_message("satellite=\"M01\" satellite-number=\"x\"", L1 + L2) + "</multi-mission-administrative-message>",
    };
    for (size_t i = 0; i < cases.size(); i++)
    {
        CAPTURE(i);
        Reader t;
        ccsds::CCSDSPacket pkt = admin_packet(cases[i]);
        t.r.work(pkt);
        CHECK(t.r.tles.empty());
    }
}

TEST_CASE("Admin message: other elements between messages are skipped")
{
    Reader t;
    const std::string xml = "<multi-mission-administrative-message><comment>hello</comment>" +
                            tle_message("satellite=\"M01\" satellite-number=\"38771\"", L1 + L2) +
                            "<comment/></multi-mission-administrative-message>";
    ccsds::CCSDSPacket pkt = admin_packet(xml);
    t.r.work(pkt);
    REQUIRE(t.r.tles.size() == 1);
    CHECK(t.r.tles[0].norad == 38771);
}
