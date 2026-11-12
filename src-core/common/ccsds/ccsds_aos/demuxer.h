#pragma once

#include "../ccsds.h"
#include <memory>
#include <vector>

namespace ccsds
{
    namespace ccsds_aos
    {
        /*
        Simple CCSDS demuxer building CCSDS frames from CADUs.
        VCID filtering must be done beforehand!
        Fork: with checkFrameCounter, each demuxer follows the frame counter of its one virtual
        channel and drops the packet in progress when frames are missing (see work()).

        Fork: strict mode (strict = true; off by default, and no SatDump decoder turns it on).
        Every packet that comes out is as long as its header says in both modes (packet length
        + 1 payload bytes, + 8 when secondaryHeaderExtendsPkt and its secondary header flag is
        set): a packet cut short by a new header, or by a frame that cannot be used, is dropped.
        Strict mode also checks a packet that runs on from an earlier frame against the frame
        in which it ends, and drops it when they disagree:
          1. When that frame has a first header pointer (a packet starts in it), the packet must
             end exactly there. One that ends before it is dropped (one that would end after it
             is dropped in both modes).
          2. When that frame says no packet starts in it (first header pointer 2047), the packet
             must fill its packet zone to the end. One that ends inside the zone is dropped.
        Either means that the length in the packet's header, or the header itself (made from the
        end of one frame and the start of the next, across a lost frame the counter check did
        not see), is wrong. The packets that start in that frame are read as in normal mode.
        A packet that starts and ends in the same frame, or that ends exactly at the end of the
        frame it started in, is not checked further.

        Fork: the state lives in Demuxer::Impl (demuxer.cpp). In builds with Rust
        (-DSATDUMP_WITH_RUST=ON) a Demuxer made while Rust is on uses the Rust demuxer
        (rust/src/aos), which gives the same packets; the C++ code is the reference and the
        fallback. If the Rust code fails (or Rust is switched off during the run), a new C++
        demuxer takes over from that CADU: what the Rust one held (the packet in progress) is
        lost, as after a lost frame. Settings the Rust code does not take (a packet zone under
        6 or over 65536 bytes, an insert zone below 0 or over 65536 bytes) use the C++ code.
        A Demuxer can be moved but not copied; a moved-from one must not be used.
    */
        class Demuxer
        {
        public:
            Demuxer(int mpdu_data_size = 884, bool hasInsertZone = false, int insertZoneSize = 2, bool secondaryHeaderExtendsPkt = false, bool checkFrameCounter = false, bool strict = false);
            ~Demuxer();
            Demuxer(Demuxer &&other) noexcept;
            Demuxer &operator=(Demuxer &&other) noexcept;
            Demuxer(const Demuxer &) = delete;
            Demuxer &operator=(const Demuxer &) = delete;

            std::vector<CCSDSPacket> work(uint8_t *cadu); // Main function

        private:
            class Impl;
            std::unique_ptr<Impl> impl;
        };
    } // namespace ccsds_aos
} // namespace ccsds
