// The CCSDS AOS packet splitter (ccsds::ccsds_aos::Demuxer). The transfer frames are built
// by hand in the MetOp / Meteor layout, as VirtualChannel in tests/golden/make_recordings.py
// does: sync marker, VCDU header with the frame counter, 2-byte insert zone, M_PDU header
// with the first header pointer, 882 bytes of packets, 128 Reed-Solomon bytes. Packets run
// on from one frame into the next.
#include "common/ccsds/ccsds.h"
#include "common/ccsds/ccsds_aos/demuxer.h"
#include "doctest.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace
{
    typedef std::vector<uint8_t> Bytes;

    const size_t ZONE = 882;         // M_PDU packet zone of a MetOp / Meteor frame
    const uint16_t NO_HEADER = 2047; // first header pointer when no packet starts in the frame
    const int IDLE_APID = 2047;

    // A space packet without secondary header: the 6-byte header and `len` payload bytes,
    // whose pattern depends on the sequence count, so no two packets are alike
    Bytes packet(int apid, int seq, size_t len)
    {
        Bytes p(6 + len);
        p[0] = uint8_t((apid >> 8) & 0x07);
        p[1] = uint8_t(apid & 0xFF);
        p[2] = uint8_t(0xC0 | ((seq >> 8) & 0x3F));
        p[3] = uint8_t(seq & 0xFF);
        p[4] = uint8_t((len - 1) >> 8);
        p[5] = uint8_t((len - 1) & 0xFF);
        for (size_t i = 0; i < len; i++)
            p[6 + i] = uint8_t(seq * 31 + i * 7 + 1);
        return p;
    }

    struct Frame
    {
        uint32_t counter;
        uint16_t fhp;
        Bytes zone;

        // The whole 1024-byte CADU: MetOp-B (spacecraft 11), virtual channel 9
        Bytes cadu() const
        {
            const uint16_t id = (1 << 14) | (11 << 6) | 9;
            const uint8_t head[14] = {0x1A, 0xCF, 0xFC, 0x1D, uint8_t(id >> 8), uint8_t(id & 0xFF),
                                      uint8_t(counter >> 16), uint8_t(counter >> 8), uint8_t(counter), 0,
                                      0, 0, uint8_t(fhp >> 8), uint8_t(fhp & 0xFF)};
            Bytes c(1024, 0);
            std::memcpy(c.data(), head, sizeof(head));
            std::memcpy(c.data() + sizeof(head), zone.data(), ZONE);
            return c;
        }
    };

    // Packets laid end to end and cut into frames
    struct Channel
    {
        Bytes stream;
        std::vector<size_t> starts; // where each packet (and the idle one at the end) starts
        std::vector<Bytes> packets; // the packets sent, without the idle one
        int seq = 0;

        // Adds a packet with `len` payload bytes; returns its number in `packets`
        size_t add(size_t len)
        {
            starts.push_back(stream.size());
            packets.push_back(packet(100, seq++, len));
            stream.insert(stream.end(), packets.back().begin(), packets.back().end());
            return packets.size() - 1;
        }

        // Adds a packet that ends where the next one should start: byte `pos` of the stream
        size_t add_until(size_t pos)
        {
            REQUIRE(pos >= stream.size() + 7);
            return add(pos - stream.size() - 6);
        }

        // Where the next frame ends (a packet from here to there spans frames)
        size_t next_frame_end() const { return (stream.size() / ZONE + 2) * ZONE; }

        // Ends the stream with an idle packet that fills the last frame, as the satellite does,
        // and cuts it into frames. (The idle packet also makes sure the last real packet comes
        // out: when the first packet to start in a frame ends exactly at the frame's end, the
        // demuxer pushes it only when the next frame arrives.)
        std::vector<Frame> frames(uint32_t first_counter = 0)
        {
            const size_t pad = ZONE - stream.size() % ZONE;
            starts.push_back(stream.size());
            Bytes idle = packet(IDLE_APID, 0, (pad >= 7 ? pad : pad + ZONE) - 6);
            stream.insert(stream.end(), idle.begin(), idle.end());
            std::vector<Frame> out;
            size_t next = 0;
            for (size_t pos = 0; pos < stream.size(); pos += ZONE)
            {
                Frame f;
                f.counter = (first_counter + uint32_t(out.size())) & 0xFFFFFF;
                f.fhp = NO_HEADER;
                while (next < starts.size() && starts[next] < pos)
                    next++;
                if (next < starts.size() && starts[next] < pos + ZONE)
                    f.fhp = uint16_t(starts[next] - pos);
                f.zone.assign(stream.begin() + pos, stream.begin() + pos + ZONE);
                out.push_back(f);
            }
            return out;
        }

        // The packets sent, leaving out those numbered in `lost`
        std::vector<Bytes> sent(const std::vector<size_t> &lost = {}) const
        {
            std::vector<Bytes> out;
            for (size_t i = 0; i < packets.size(); i++)
                if (std::find(lost.begin(), lost.end(), i) == lost.end())
                    out.push_back(packets[i]);
            return out;
        }
    };

    // Feeds the frames to a MetOp / Meteor layout demuxer, with the frame counter check the
    // MetOp decoder turns on, and returns what comes out (header and payload of each packet),
    // without idle packets. Every packet must be whole: as long as its header says.
    // Fork: strict mode (demuxer.h) and no counter check can be asked for too.
    std::vector<Bytes> run(const std::vector<Frame> &frames, bool strict = false, bool check_counter = true)
    {
        ccsds::ccsds_aos::Demuxer demux(882, true, 2, false, check_counter, strict);
        std::vector<Bytes> out;
        for (const Frame &f : frames)
        {
            Bytes c = f.cadu();
            for (ccsds::CCSDSPacket &p : demux.work(c.data()))
            {
                CAPTURE(out.size());
                CHECK(p.payload.size() == size_t(p.header.packet_length) + 1);
                if (p.header.apid == IDLE_APID)
                    continue;
                Bytes b(p.header.raw, p.header.raw + 6);
                b.insert(b.end(), p.payload.begin(), p.payload.end());
                out.push_back(b);
            }
        }
        return out;
    }

    void check_same(const std::vector<Bytes> &out, const std::vector<Bytes> &expected)
    {
        CHECK(out.size() == expected.size());
        for (size_t i = 0; i < std::min(out.size(), expected.size()); i++)
        {
            CAPTURE(i);
            CHECK(out[i].size() == expected[i].size());
            CHECK(out[i] == expected[i]);
        }
    }

    // A, P, Q, R, S, T at stream bytes 0, 300, 1300, 3300, 3400, 3900. P runs from frame 0
    // into frame 1 (to byte 418 of it); Q starts there and fills all of frame 2.
    struct SpliceLayout
    {
        Channel ch;
        size_t p, q;
        std::vector<Frame> frames;
        SpliceLayout()
        {
            ch.add(294);
            p = ch.add(994);
            q = ch.add(1994);
            ch.add(94);
            ch.add(494);
            ch.add(50);
            frames = ch.frames();
            REQUIRE(frames.size() == 5u);
            REQUIRE(frames[1].fhp == 418);
            REQUIRE(frames[2].fhp == NO_HEADER);
        }
    };
} // namespace

TEST_CASE("AOS demuxer: intact frames give exactly the packets that were sent")
{
    Channel ch;
    ch.add(100); // several packets in one frame
    ch.add(200);
    ch.add(1);
    // A header cut after k bytes, for every k (k = 6: the whole header is the last 6 bytes)
    for (size_t k = 1; k <= 6; k++)
    {
        ch.add_until(ch.next_frame_end() - k);
        ch.add(40 + 50 * k);
    }
    ch.add(2000); // over three frames
    // The first packet to start in a frame ends exactly at the frame's end
    ch.add_until(ch.next_frame_end() + 300);
    ch.add(ZONE - 300 - 6);
    // The next frame starts with a header; its last packet ends exactly at its end
    ch.add(100);
    ch.add(ZONE - 106 - 6);
    // A header cut after 2 bytes, whose packet ends exactly at the end of the next frame
    ch.add_until(ch.next_frame_end() - 2);
    ch.add(ZONE - 4);
    // Many more, short and long
    uint32_t x = 1;
    for (int i = 0; i < 300; i++)
    {
        x = x * 1103515245u + 12345u;
        ch.add(1 + (x >> 16) % (i % 3 == 0 ? 1500 : 120));
    }
    check_same(run(ch.frames()), ch.sent());
}

TEST_CASE("AOS demuxer: a packet spanning frames comes out whole")
{
    Channel ch;
    ch.add(500);
    ch.add(3000); // the rest of frame 0, all of frames 1 to 3, part of frame 4
    ch.add(10);
    check_same(run(ch.frames()), ch.sent());
}

TEST_CASE("AOS demuxer: a header cut across two frames, at each place")
{
    for (size_t k = 1; k <= 6; k++)
    {
        CAPTURE(k);
        Channel ch;
        ch.add_until(ZONE - k); // the next header starts k bytes before the end of frame 0
        ch.add(300);
        ch.add(50);
        std::vector<Frame> frames = ch.frames();
        REQUIRE(frames[0].fhp == 0);
        REQUIRE(size_t(frames[1].fhp) == 300 + 6 - k);
        check_same(run(frames), ch.sent());
    }
}

TEST_CASE("AOS demuxer: the 24-bit frame counter wraps without a false gap")
{
    Channel ch;
    ch.add(1500);
    ch.add(1500); // over the frames counted 0xFFFFFF, 0 and 1
    ch.add(20);
    std::vector<Frame> frames = ch.frames(0xFFFFFE);
    REQUIRE(frames.size() == 4u);
    REQUIRE(frames[2].counter == 0u);
    check_same(run(frames), ch.sent());
}

TEST_CASE("AOS demuxer: a lost frame drops the packet in progress, and later packets come out whole")
{
    SUBCASE("the packet in progress would be finished from the frame after the gap")
    {
        // Frame 1 is lost. The old code finished P with the first 418 bytes of frame 2 (the
        // middle of Q) and pushed it. Q started in the lost frame, so it is lost too.
        SpliceLayout s;
        s.frames.erase(s.frames.begin() + 1);
        check_same(run(s.frames), s.ch.sent({s.p, s.q}));
    }
    SUBCASE("a header cut across frames, then a gap")
    {
        // H's header starts 3 bytes before the end of frame 0; frame 1 is lost. The old code
        // made a header from those 3 bytes and 3 bytes of frame 2, and pushed that packet.
        Channel ch;
        ch.add_until(ZONE - 3);
        const size_t h = ch.add(1000); // to byte 121 of frame 2
        ch.add(200);
        ch.add(300);
        std::vector<Frame> frames = ch.frames();
        REQUIRE(frames[2].fhp == 121);
        // Frame 2's first bytes (the end of H, lost anyway) are set so that a header made
        // from 3 old and 3 new bytes reads as an 11-byte packet ending before frame 2's
        // pointer: only dropping the cut header at the gap keeps it out
        frames[2].zone[1] = 0;
        frames[2].zone[2] = 10;
        frames.erase(frames.begin() + 1);
        check_same(run(frames), ch.sent({h}));
    }
    SUBCASE("a packet that ended exactly at the end of the last frame before the gap still comes out")
    {
        Channel ch;
        ch.add_until(ZONE + 200);
        ch.add(ZONE - 200 - 6);        // the first packet to start in frame 1; ends with it
        const size_t c = ch.add(2000); // frames 2 to 4: frame 2 is lost
        ch.add(100);
        std::vector<Frame> frames = ch.frames();
        REQUIRE(frames[2].fhp == 0);
        frames.erase(frames.begin() + 2);
        check_same(run(frames), ch.sent({c}));
    }
}

TEST_CASE("AOS demuxer: a damaged first header pointer gives no cut-off packets and no stale bytes")
{
    SUBCASE("pointing before the end of the packet in progress")
    {
        // P really ends at byte 418 of frame 1. The old code pushed P with 401 bytes from
        // frame 1 (977 of its 994), the last one taken from the "header" at 400. That header
        // reads as a packet with 60660 payload bytes, which the good pointer in frame 3 cuts
        // short: it is dropped too, as are P and Q (whose header the bad pointer skipped).
        SpliceLayout s;
        s.frames[1].fhp = 400;
        check_same(run(s.frames), s.ch.sent({s.p, s.q}));
    }
    SUBCASE("pointing into the rest of a header cut across frames")
    {
        // H's header has 1 byte in frame 0 and 5 in frame 1, so its payload starts at byte 5.
        // A pointer of 2 made the old code write 3 - 5 = -2 bytes and push H empty.
        Channel ch;
        ch.add_until(ZONE - 1);
        const size_t h = ch.add(300);
        const size_t x = ch.add(1000); // from byte 305 of frame 1 to byte 429 of frame 2
        ch.add(200);
        ch.add(20);
        std::vector<Frame> frames = ch.frames();
        REQUIRE(frames[1].fhp == 305);
        REQUIRE(frames[2].fhp == 429);
        frames[1].fhp = 2;
        check_same(run(frames), ch.sent({h, x}));
    }
    SUBCASE("saying no packet starts, after a header cut across frames")
    {
        // H's header has 1 byte in frame 0 and 5 in frame 1; H ends at byte 879 of frame 1,
        // where X starts. Frame 1 wrongly says no packet starts in it. The old code wrote 877
        // bytes into H (3 too many), never pushed it, and put those bytes in front of R.
        // Now H comes out whole and only X, whose header was missed, is lost.
        Channel ch;
        ch.add_until(ZONE - 1);
        ch.add(874);
        const size_t x = ch.add(100); // to byte 103 of frame 2
        ch.add(200);
        ch.add(20);
        std::vector<Frame> frames = ch.frames();
        REQUIRE(frames[1].fhp == 879);
        REQUIRE(frames[2].fhp == 103);
        frames[1].fhp = NO_HEADER;
        check_same(run(frames), ch.sent({x}));
    }
    SUBCASE("pointing outside the packet zone after a header cut across frames")
    {
        // H's header starts 3 bytes before the end of frame 0; frame 1's pointer is out of
        // range, so H is lost. The old code then completed the header with 3 bytes of frame 2,
        // which are set here so that it reads as a short packet ending before frame 2's
        // pointer, and pushed it.
        Channel ch;
        ch.add_until(ZONE - 3);
        const size_t h = ch.add(1000); // to byte 121 of frame 2
        ch.add(200);
        ch.add(300);
        std::vector<Frame> frames = ch.frames();
        REQUIRE(frames[2].fhp == 121);
        frames[2].zone[1] = 0;
        frames[2].zone[2] = 10;
        frames[1].fhp = 1000;
        check_same(run(frames), ch.sent({h}));
    }
    SUBCASE("pointing outside the packet zone")
    {
        // The old code skipped frame 1 but kept P going, so P was finished with the first
        // 418 bytes of frame 2 (the middle of Q). Now the frame counts as lost.
        SpliceLayout s;
        s.frames[1].fhp = 1000;
        check_same(run(s.frames), s.ch.sent({s.p, s.q}));
    }
}

// Fork: strict mode (rules 1 and 2 in demuxer.h). It changes nothing on intact frames and
// drops a packet that runs on from an earlier frame when the frame it ends in disagrees.
TEST_CASE("AOS demuxer: strict mode gives the same packets on intact frames, with and without the counter check")
{
    Channel ch;
    ch.add(100);
    ch.add(1);
    for (size_t k = 1; k <= 6; k++)
    {
        ch.add_until(ch.next_frame_end() - k);
        ch.add(40 + 50 * k);
    }
    ch.add(3000);                            // over several frames
    ch.add_until(ch.next_frame_end() + 300); // the first packet to start in a frame...
    ch.add(ZONE - 300 - 6);                  // ...ends exactly at its end
    ch.add_until(ch.next_frame_end() - 2);   // a cut header, whose packet ends exactly
    ch.add(ZONE - 4);                        // at the end of the next frame
    ch.add_until(ch.next_frame_end());       // ends exactly at the end of a frame where no
                                             // packet starts (strict rule 2's edge)
    ch.add(65536);                           // the longest packet a header can describe
    const std::vector<Frame> frames = ch.frames(0xFFFFFE); // the counter wraps too
    for (bool strict : {false, true})
        for (bool counter : {false, true})
        {
            CAPTURE(strict);
            CAPTURE(counter);
            check_same(run(frames, strict, counter), ch.sent());
        }
}

TEST_CASE("AOS demuxer: strict mode drops a packet that ends before the first header of the frame it ends in")
{
    // P's header (at byte 300 of frame 0) says 10 bytes fewer than it has, so it ends 10 bytes
    // before frame 1's first header pointer. Normal mode pushes P with its first 984 bytes.
    SpliceLayout s;
    const uint16_t length = uint16_t((s.frames[0].zone[304] << 8 | s.frames[0].zone[305]) - 10);
    s.frames[0].zone[304] = uint8_t(length >> 8);
    s.frames[0].zone[305] = uint8_t(length & 0xFF);
    const std::vector<Bytes> normal = run(s.frames);
    REQUIRE(normal.size() == s.ch.packets.size());
    CHECK(normal[1].size() == 6 + 984u);
    CHECK(normal[2] == s.ch.packets[s.q]);
    check_same(run(s.frames, true), s.ch.sent({s.p}));
}

TEST_CASE("AOS demuxer: strict mode drops a packet that ends inside a frame where no packet starts")
{
    SUBCASE("a first header pointer damaged into 2047")
    {
        // P ends at byte 418 of frame 1, where Q starts, but frame 1 says no packet starts in
        // it. Normal mode pushes P (it is whole) and misses Q's header.
        SpliceLayout s;
        s.frames[1].fhp = NO_HEADER;
        check_same(run(s.frames), s.ch.sent({s.q}));
        check_same(run(s.frames, true), s.ch.sent({s.p, s.q}));
    }
    SUBCASE("a frame received twice, without the counter check")
    {
        // Frame 2 (the middle of Q, no packet starts in it) comes twice. Without the counter
        // check, normal mode finishes Q with the copy and pushes it wrong; strict mode drops
        // it, as Q then ends inside the copy. With the check, both drop it at the copy.
        SpliceLayout s;
        const Frame again = s.frames[2];
        s.frames.insert(s.frames.begin() + 3, again);
        const std::vector<Bytes> normal = run(s.frames, false, false);
        REQUIRE(normal.size() == s.ch.packets.size());
        CHECK(normal[2] != s.ch.packets[s.q]);
        check_same(run(s.frames, true, false), s.ch.sent({s.q}));
        check_same(run(s.frames, false, true), s.ch.sent({s.q}));
        check_same(run(s.frames, true, true), s.ch.sent({s.q}));
    }
}
