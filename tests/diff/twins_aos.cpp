// Fork part "aos": the CCSDS AOS demuxer as a twin, aos_demux. The C++ side is
// ccsds::ccsds_aos::Demuxer as SatDump uses it, made with Rust paused (run.cpp), so it is its
// C++ code; the other side is the Rust demuxer through rust_aos.h, with no C++ fallback, so a
// Rust failure shows as a status (STATUS_PANIC). rust/tests/difftest/aos.rs replays the oracles
// with the Rust demuxer alone.
//
// Each chunk of a case is one CADU: a short one is filled up with zeros to the bytes the
// demuxer reads (up to the end of the packet zone), and longer ones are cut there. Each packet
// that comes out is a record: the CADU it came out of, the demuxer variant (corpus files run
// through four), the virtual channel when routing (else -1), the header's fields and bytes,
// and the payload. Settings the Rust code refuses give STATUS_BAD_ARGUMENT on both sides
// (the C++ side asks rust_aos.h's supports()). The cases are in twins_aos_cases.cpp.
#include "common/ccsds/ccsds.h"
#include "common/ccsds/ccsds_aos/demuxer.h"
#include "rust_aos.h"
#include "rust_bridge.h"
#include "twins_aos.h"
#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <utility>

namespace satdump::diff
{
    namespace
    {
        using Packets = std::vector<ccsds::CCSDSPacket>;

        struct Settings
        {
            int mpdu;
            bool has_iz;
            int iz;
            bool ext, counter, strict;
        };

        // The settings of a case, or of a corpus file's four demuxers
        std::vector<Settings> variants(const Case &c)
        {
            if (c.params.count("mpdu") == 0)
                return {{882, true, 2, false, false, false}, {882, true, 2, false, false, true},
                        {882, true, 2, false, true, false}, {882, true, 2, false, true, true}};
            return {{int(c.param("mpdu", 0)), c.param("has_iz", 0) != 0, int(c.param("iz", 0)), c.param("ext", 0) != 0,
                     c.param("counter", 0) != 0, c.param("strict", 0) != 0}};
        }

        // The C++ demuxer
        struct CppSide
        {
            std::unique_ptr<ccsds::ccsds_aos::Demuxer> d;
            int64_t make(const Settings &s)
            {
                if (!satdump::rust::AosDemuxer::supports(s.mpdu, s.has_iz, s.iz))
                    return satdump::rust::STATUS_BAD_ARGUMENT;
                d = std::make_unique<ccsds::ccsds_aos::Demuxer>(s.mpdu, s.has_iz, s.iz, s.ext, s.counter, s.strict);
                return satdump::rust::STATUS_OK;
            }
            bool work(uint8_t *cadu, Packets &out)
            {
                out = d->work(cadu);
                return true;
            }
        };

        // The Rust demuxer
        struct RustSide
        {
            std::unique_ptr<satdump::rust::AosDemuxer> d;
            int64_t make(const Settings &s)
            {
                d = satdump::rust::AosDemuxer::create(s.mpdu, s.has_iz, s.iz, s.ext, s.counter, s.strict);
                if (d)
                    return satdump::rust::STATUS_OK;
                // The bridge switches Rust off after a failure, which tells the two apart
                return satdump::rust::enabled() ? satdump::rust::STATUS_BAD_ARGUMENT : satdump::rust::STATUS_PANIC;
            }
            bool work(uint8_t *cadu, Packets &out)
            {
                out.clear();
                return d->work(cadu, out);
            }
        };

        Record record(int64_t cadu, size_t variant, int vcid, const ccsds::CCSDSPacket &p)
        {
            const ccsds::CCSDSHeader &h = p.header;
            return {{"cadu", Value::integer(cadu)},
                    {"variant", Value::integer(int64_t(variant))},
                    {"vcid", Value::integer(vcid)},
                    {"apid", Value::integer(h.apid)},
                    {"version", Value::integer(h.version)},
                    {"type", Value::integer(h.type)},
                    {"secondary", Value::integer(h.secondary_header_flag)},
                    {"sequence_flag", Value::integer(h.sequence_flag)},
                    {"sequence", Value::integer(h.packet_sequence_count)},
                    {"length", Value::integer(h.packet_length)},
                    {"header", Value::data(std::vector<uint8_t>(h.raw, h.raw + 6))},
                    {"payload", Value::data(p.payload)}};
        }

        template <typename Side>
        Outcome demux(const Case &c)
        {
            Outcome o;
            const std::vector<Settings> v = variants(c);
            const bool route = c.param("route", c.params.count("mpdu") == 0 ? 1 : 0) != 0;
            {
                // Refused settings are refused before any CADU
                Side probe;
                o.status = probe.make(v[0]);
                if (o.status != satdump::rust::STATUS_OK)
                    return o;
            }
            const size_t bytes = size_t(12 + (v[0].has_iz ? v[0].iz : 0) + v[0].mpdu);
            std::map<std::pair<size_t, int>, Side> demuxers; // by variant and virtual channel
            std::vector<uint8_t> cadu(bytes);
            Packets packets;
            size_t pos = 0;
            int64_t index = 0;
            for (size_t n : chunks_of(c))
            {
                if (n > 0)
                {
                    std::fill(cadu.begin(), cadu.end(), 0);
                    std::memcpy(cadu.data(), c.input.data() + pos, std::min(n, bytes));
                    const int vcid = route ? cadu[5] & 0x3F : -1;
                    for (size_t k = 0; k < v.size(); k++)
                    {
                        auto it = demuxers.find({k, vcid});
                        if (it == demuxers.end())
                        {
                            Side s;
                            if ((o.status = s.make(v[k])) != satdump::rust::STATUS_OK)
                                return o;
                            it = demuxers.emplace(std::make_pair(k, vcid), std::move(s)).first;
                        }
                        if (!it->second.work(cadu.data(), packets))
                        {
                            o.status = satdump::rust::STATUS_PANIC;
                            return o;
                        }
                        for (const ccsds::CCSDSPacket &p : packets)
                            o.records.push_back(record(index, k, vcid, p));
                    }
                }
                pos += n;
                index++;
            }
            return o;
        }
    } // namespace

    void add_aos_twins(std::vector<Twin> &twins)
    {
        Twin t;
        t.name = "aos_demux";
        t.about = "CCSDS AOS demuxer fed CADU by CADU (ccsds::ccsds_aos::Demuxer's C++ code against rust/src/aos), strict mode off and on";
        t.cpp = demux<CppSide>;
        t.rust = demux<RustSide>;
        t.count = aos::case_count;
        t.make = aos::make_case;
        t.stream = true;
        t.group = aos::layout_group; // satdump-diff run prints the cases and CADUs of each layout
        // Corpus files are read as 1024-byte CADUs (the golden MetOp and Meteor recordings)
        t.corpus_frame = 1024;
        twins.push_back(t);
    }
} // namespace satdump::diff
