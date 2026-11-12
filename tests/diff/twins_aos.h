#pragma once

// Fork part "aos": what twins_aos.cpp (the aos_demux twin) and twins_aos_cases.cpp (its cases)
// share.
#include "diff.h"
#include <string>
#include <vector>

namespace satdump::diff::aos
{
    // A demuxer's settings as SatDump's callers give them (see LAYOUTS in twins_aos_cases.cpp)
    struct Layout
    {
        const char *name;
        int mpdu;     // bytes in the packet zone
        bool has_iz;  // an insert zone...
        int iz;       // ...of this many bytes
        bool ext;     // secondaryHeaderExtendsPkt
        bool counter; // checkFrameCounter
    };
    const std::vector<Layout> &layouts();

    // A case's settings are its parameters: mpdu, has_iz, iz, ext, counter, strict and route
    // (one demuxer per virtual channel, as the decoders do). A case without "mpdu" is a corpus
    // file (the golden MetOp and Meteor recordings, or any file): 1024-byte frames in the
    // MetOp / Meteor layout, routed by virtual channel, through four demuxers each (counter
    // check off and on, times strict mode off and on).
    void set_params(Case &c, const Layout &l, bool counter, bool strict, bool route);
    // The group a case counts under in the run summary (diff.h, Twin::group): "call site",
    // "other layout" or "refused layout" and the names of the layouts with its settings (the
    // frame counter check aside: lrpt/metop), or a corpus file
    std::string layout_group(const Case &c);

    uint64_t case_count(const Plan &p);
    Case make_case(const Plan &p, uint64_t i);
} // namespace satdump::diff::aos
