// Fork part "aos": the cases of the aos_demux twin (twins_aos.cpp). Each case is a stream of
// CADUs, one chunk each, made by a structured generator (packets laid end to end and cut into
// M_PDUs as a satellite does it), so its oracle lines carry the bytes (inline_input).
//
// A normal plan has, in order:
//   - the standard stream in every layout below, strict mode off and on: packets with a
//     1-byte payload, a header cut at every place (0 to 6 of its bytes in the first frame), a
//     packet that ends exactly at the end of a zone, one over several zones, an idle packet, a
//     cut header whose packet ends at the end of the next zone, and the 24-bit frame counter
//     wrapping;
//   - in six layouts, strict mode off and on, the standard stream with one damage each: frame
//     2's first header pointer set to 0, the zone's last byte, 2046, 2047, the zone size, one
//     more, or 2000; a gap, a repeat or a step back in the frame counter; frame 2 dropped or
//     received twice; the last CADU cut short; header lengths that lie; idle packets only;
//   - packets of the largest length (65536 bytes after the header, and 65544 with a secondary
//     header in a layout that adds its 8 bytes) in three layouts, strict mode off and on;
//   - SD_DIFF_CASES seeded streams in any layout: packets from 1 byte to 65536, filling zones
//     exactly or ending 0 to 6 bytes before their end, idle ones, lying lengths, damaged first
//     header pointers and frame counters, two virtual channels mixed, then the framework's
//     mutations (bit flips, changed bytes, truncation, splices, dropped or repeated CADUs).
//     Streams in the layouts SatDump uses are 300 to 24300 bytes long (60000 to 80000 bytes
//     with the longest packets, in one case of 50); those in the small made-up zones (under
//     SMALL_ZONE bytes) are 10 to 200 frames long and get no packets over 3000 bytes, so that
//     most of a deep run's frames are not 18-byte ones (satdump-diff run prints the cases and
//     CADUs of each layout).
// A small plan (the committed oracle) has ten short edge cases and SD_DIFF_CASES (6) seeded
// ones, in small layouts.
#include "twins_aos.h"
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace satdump::diff::aos
{
    namespace
    {
        // Every call site's settings, then others: what BitView's user-set demuxer may get,
        // and the edges of the Rust range; the last four are refused by the Rust code (the C++
        // demuxer is used), where both sides must give STATUS_BAD_ARGUMENT
        const std::vector<Layout> LAYOUTS = {
            {"default", 884, false, 2, false, false},   // EOS (9 demuxers), MetOp-SG, xRIT (GOES, GK-2A LRIT, Elektro), src-testing
            {"fy4_lrit", 1012, false, 2, false, false}, // FY-4 LRIT (xRIT)
            {"gk2a_uhrit", 2034, false, 0, true, false},
            {"goes_grb", 2034, false, 2, false, false},
            {"gcom1", 1092, false, 2, false, false},
            {"ldcm", 1022, true, 0, false, false},  // Landsat LDCM
            {"lrpt", 882, true, 2, false, false},   // Meteor LRPT, FY-3, SARAL ARGOS, ORB
            {"metop", 882, true, 2, false, true},   // MetOp (7 demuxers)
            {"npp", 884, true, 0, false, false},    // JPSS in NPP mode, GOES-R raw
            {"jpss", 1094, true, 9, false, false},  // JPSS
            {"hinode", 880, true, 0, false, false}, // Hinode
            {"wsfm", 1010, true, 0, false, false},  // WSF-M
            {"bitview_ext", 884, true, 4, true, false},
            {"metop_ext", 882, true, 2, true, true},
            {"tiny", 6, false, 0, false, true},
            {"small", 7, true, 1, true, false},
            {"compact", 40, true, 2, false, true},
            {"compact_ext", 100, false, 0, true, false},
            {"wide", 2047, false, 0, false, true},  // a first header pointer of 2046 is in the zone
            {"wider", 2100, true, 3, false, false}, // packets starting at 2047 or later cannot be pointed to
            {"refused_5", 5, false, 0, false, false},
            {"refused_0", 0, false, 0, false, false},
            {"refused_negative_iz", 884, true, -1, false, false},
            {"refused_70000", 70000, false, 0, false, false},
        };
        constexpr size_t CALL_SITES = 12, SUPPORTED = 20;
        const std::vector<size_t> FOCUS = {7, 0, 2, 9, 14, 18}; // metop, default, gk2a_uhrit, jpss, tiny, wide
        const std::vector<size_t> SMALL_LAYOUTS = {14, 15, 16, 17};
        const std::vector<size_t> MAX_LENGTH = {7, 13, 16}; // metop, metop_ext, compact
        constexpr int SMALL_ZONE = 256;
        constexpr uint16_t NO_HEADER = 2047, IDLE_APID = 2047;
        constexpr uint64_t SEEDED = 0x616F73ULL, EDGES = 0x65646765ULL; // generators of their own

        size_t find(const char *name)
        {
            for (size_t i = 0; i < LAYOUTS.size(); i++)
                if (std::string(LAYOUTS[i].name) == name)
                    return i;
            throw std::logic_error(std::string("no layout ") + name);
        }

        // Space packets laid end to end, as a virtual channel sends them
        class Packets
        {
        public:
            Packets(Rng &r, const Layout &l, bool long_ones) : r(r), l(l), zone(size_t(l.mpdu)), long_ones(long_ones) {}

            size_t size() const { return bytes.size(); }

            // A packet with `total` bytes after its header, which says so (counting the secondary
            // header's 8 bytes when the layout adds them) unless `lie` is not 0. Its secondary
            // header flag is `flag` (0 or 1), or picked at random (-1)
            void add(size_t total, bool idle = false, int lie = 0, int flag = -1)
            {
                total = std::max<size_t>(total, 1);
                bool secondary = flag >= 0 ? flag == 1 : !idle && r.below(3) == 0;
                if (l.ext && secondary && total < 9)
                    secondary = false;
                const size_t extra = l.ext && secondary ? 8 : 0;
                total = std::min<size_t>(total, 65536 + extra);
                const int64_t said = std::clamp<int64_t>(int64_t(total - extra) - 1 + lie, 0, 65535);
                const uint16_t apid = idle ? IDLE_APID : uint16_t(r.below(2047));
                const uint16_t seq = uint16_t(r.below(1 << 14));
                const uint8_t version = r.below(8) == 0 ? uint8_t(r.below(8)) : 0;
                const uint8_t flags = r.below(8) == 0 ? uint8_t(r.below(4)) : 3;
                starts.push_back(bytes.size());
                bytes.push_back(uint8_t(version << 5 | uint8_t(r.below(2)) << 4 | uint8_t(secondary) << 3 | apid >> 8));
                bytes.push_back(uint8_t(apid & 0xFF));
                bytes.push_back(uint8_t(flags << 6 | seq >> 8));
                bytes.push_back(uint8_t(seq & 0xFF));
                bytes.push_back(uint8_t(said >> 8));
                bytes.push_back(uint8_t(said & 0xFF));
                for (size_t i = 0; i < total; i++)
                    bytes.push_back(idle ? 0x55 : uint8_t(r.next()));
            }

            // A packet that ends `before` bytes before the end of a zone, where the next header
            // then starts (cut after `before` bytes, or at the start of the next zone for 0)
            void add_until(size_t before)
            {
                const size_t need = bytes.size() + 7 + before;
                add((need + zone - 1) / zone * zone - before - bytes.size() - 6);
            }

            void add_random(bool lies)
            {
                const int lie = lies && r.below(6) == 0 ? int(r.below(21)) - 10 : 0;
                switch (r.below(12))
                {
                case 0:
                    add(1, false, lie);
                    break;
                case 1:
                    add(2 + r.below(15), false, lie);
                    break;
                case 2:
                case 3:
                    add(17 + r.below(284), false, lie);
                    break;
                case 4:
                    add_until(0);
                    break;
                case 5:
                    add_until(size_t(r.below(7)));
                    break;
                case 6:
                    add(zone * (1 + r.below(4)) + r.below(zone), false, lie);
                    break;
                case 7:
                    add(1 + r.below(std::min<size_t>(zone, 2000)), true);
                    break;
                case 8:
                    add(long_ones ? 60000 + r.below(5537) : 300 + r.below(2700), false, lie);
                    break;
                default:
                    add(1 + r.below(120), false, lie);
                    break;
                }
            }

            // The standard stream (see the top of this file)
            void standard()
            {
                add(1);
                add(5);
                for (size_t k = 0; k <= 6; k++)
                {
                    add_until(k);
                    add(1 + r.below(40));
                }
                add_until(0);
                add(3 * zone + 17);
                add(1 + r.below(zone), true);
                add(1);
                add_until(2);
                add(std::max<size_t>(zone, 5) - 4);
                add(60);
            }

            // The zones and their first header pointers, after an idle packet to the end of a
            // zone as the satellite sends
            std::vector<std::pair<uint16_t, std::vector<uint8_t>>> zones()
            {
                const size_t room = zone - bytes.size() % zone;
                if (room != zone)
                    add(room >= 7 ? room - 6 : room + zone - 6, true);
                std::vector<std::pair<uint16_t, std::vector<uint8_t>>> out;
                size_t next = 0;
                for (size_t pos = 0; pos < bytes.size(); pos += zone)
                {
                    while (next < starts.size() && starts[next] < pos)
                        next++;
                    uint16_t fhp = NO_HEADER;
                    if (next < starts.size() && starts[next] < pos + zone && starts[next] - pos < NO_HEADER)
                        fhp = uint16_t(starts[next] - pos);
                    out.push_back({fhp, std::vector<uint8_t>(bytes.begin() + pos, bytes.begin() + pos + zone)});
                }
                return out;
            }

        private:
            Rng &r;
            const Layout &l;
            size_t zone;
            bool long_ones;
            std::vector<uint8_t> bytes;
            std::vector<size_t> starts;
        };

        using Zones = std::vector<std::pair<uint16_t, std::vector<uint8_t>>>;

        // The first header pointer edits: 0, the zone's last byte, 2046, 2047, the zone size,
        // one more, 2000 (11 bits are sent)
        uint16_t pointer_edit(const Layout &l, uint64_t which)
        {
            const int values[] = {0, l.mpdu - 1, 2046, 2047, l.mpdu, l.mpdu + 1, 2000};
            return uint16_t(values[which % 7] & 0x7FF);
        }

        // One CADU: sync marker, VCDU header (spacecraft 43), insert zone, M_PDU header (its 5
        // spare bits set when `noise`), the zone
        std::vector<uint8_t> cadu(Rng &r, const Layout &l, uint8_t vcid, uint32_t counter, uint16_t fhp,
                                  const std::vector<uint8_t> &zone, bool noise)
        {
            std::vector<uint8_t> c = {0x1A, 0xCF, 0xFC, 0x1D, 0x4A, uint8_t(0xC0 | (vcid & 0x3F)),
                                      uint8_t(counter >> 16), uint8_t(counter >> 8), uint8_t(counter), 0};
            for (int i = 0; i < (l.has_iz ? l.iz : 0); i++)
                c.push_back(uint8_t(r.next()));
            c.push_back(uint8_t((noise ? r.below(32) : 0) << 3 | (fhp >> 8 & 7)));
            c.push_back(uint8_t(fhp & 0xFF));
            c.insert(c.end(), zone.begin(), zone.end());
            return c;
        }

        // Frame counters from `start`, one up per frame; with `damage`, gaps, repeats and steps back
        std::vector<uint32_t> counters(Rng &r, size_t n, uint32_t start, bool damage)
        {
            std::vector<uint32_t> out;
            uint32_t v = start;
            for (size_t i = 0; i < n; i++)
            {
                out.push_back(v & 0xFFFFFF);
                const uint64_t d = damage ? r.below(12) : 3;
                if (d == 0)
                    v += uint32_t(2 + r.below(8)); // a gap
                else if (d == 2)
                    v += 0xFFFFFF; // one back
                else if (d != 1)
                    v += 1; // (1: the same again)
            }
            return out;
        }

        Case make(const std::string &id, const Layout &l, bool counter, bool strict, bool route,
                  const std::vector<std::vector<uint8_t>> &cadus)
        {
            Case c;
            c.id = id;
            c.inline_input = true;
            for (const std::vector<uint8_t> &one : cadus)
            {
                c.input.insert(c.input.end(), one.begin(), one.end());
                c.chunks.push_back(one.size());
            }
            c.length = c.input.size();
            set_params(c, l, counter, strict, route);
            return c;
        }

        bool refused(const Layout &l) { return &l >= &LAYOUTS[SUPPORTED]; }

        // A refused layout's case: a few CADU-sized pieces of random bytes, which neither side reads
        Case refused_case(const std::string &id, Rng &r, const Layout &l, bool counter, bool strict)
        {
            std::vector<std::vector<uint8_t>> pieces(3, std::vector<uint8_t>(64));
            for (auto &p : pieces)
                for (uint8_t &b : p)
                    b = uint8_t(r.next());
            return make(id, l, counter, strict, false, pieces);
        }

        // Zones made into CADUs of one virtual channel
        std::vector<std::vector<uint8_t>> cadus_of(Rng &r, const Layout &l, const Zones &zones, uint8_t vcid,
                                                   const std::vector<uint32_t> &count, bool noise)
        {
            std::vector<std::vector<uint8_t>> out;
            for (size_t i = 0; i < zones.size(); i++)
                out.push_back(cadu(r, l, vcid, count[i], zones[i].first, zones[i].second, noise));
            return out;
        }

        enum Damage
        {
            NONE = -1,
            POINTER = 0, // ... to POINTER + 6: the seven first header pointer edits
            COUNTER_GAP = 7,
            COUNTER_REPEAT,
            COUNTER_BACK,
            DROPPED,
            REPEATED,
            TRUNCATED,
            LIES,
            IDLE_ONLY,
            DAMAGES
        };
        const char *const DAMAGE_NAMES[] = {"fhp-0", "fhp-last", "fhp-2046", "fhp-2047", "fhp-zone", "fhp-zone+1", "fhp-2000",
                                            "counter-gap", "counter-repeat", "counter-back", "dropped", "repeated",
                                            "truncated", "lies", "idle-only"};

        // The standard stream in a layout, with one damage
        Case standard_case(Rng &r, size_t layout, bool strict, int damage)
        {
            const Layout &l = LAYOUTS[layout];
            const bool counter = damage >= COUNTER_GAP && damage <= COUNTER_BACK ? true : l.counter;
            std::string id = std::string(damage == NONE ? "standard" : DAMAGE_NAMES[damage]) + "-" + l.name + (strict ? "-strict" : "");
            if (refused(l))
                return refused_case(id, r, l, counter, strict);
            Packets m(r, l, false);
            if (damage == IDLE_ONLY)
                for (int i = 0; i < 12; i++)
                    m.add(1 + r.below(size_t(2 * l.mpdu)), true);
            else if (damage == LIES)
                for (int i = 0; i < 40; i++)
                    m.add_random(true);
            else
                m.standard();
            Zones zones = m.zones();
            if (damage >= POINTER && damage < COUNTER_GAP)
                zones.at(2).first = pointer_edit(l, uint64_t(damage));
            std::vector<uint32_t> count = counters(r, zones.size(), 0xFFFFFE, false);
            if (damage == COUNTER_GAP)
                for (size_t i = 2; i < count.size(); i++)
                    count[i] = (count[i] + 3) & 0xFFFFFF;
            if (damage == COUNTER_REPEAT || damage == COUNTER_BACK)
                for (size_t i = 2; i < count.size(); i++)
                    count[i] = (count[i] - (damage == COUNTER_REPEAT ? 1 : 2)) & 0xFFFFFF;
            Case c = make(id, l, counter, strict, false, cadus_of(r, l, zones, 9, count, false));
            if (damage == DROPPED)
                drop_chunk(c, 2);
            if (damage == REPEATED)
                dup_chunk(c, 2);
            if (damage == TRUNCATED)
                truncate(c, c.input.size() - c.chunks.back() / 2);
            return c;
        }

        // Packets of the largest length, 65536 bytes after the header (its length field 65535),
        // without and with a secondary header (65544 bytes where the layout adds its 8), one of
        // them after a header cut across frames
        Case max_length_case(Rng &r, size_t layout, bool strict)
        {
            const Layout &l = LAYOUTS[layout];
            Packets m(r, l, false);
            m.add(5);
            m.add(65536, false, 0, 0);
            m.add(1);
            m.add(65536 + 8, false, 0, 1);
            m.add_until(3);
            m.add(65536 + 8, false, 0, 1);
            m.add(10);
            Zones zones = m.zones();
            return make(std::string("max-length-") + l.name + (strict ? "-strict" : ""), l, l.counter, strict, false,
                        cadus_of(r, l, zones, 9, counters(r, zones.size(), 0xFFFFF0, false), false));
        }

        // The committed oracle's edge cases: short, in small layouts, and one in MetOp's
        Case small_edge(Rng &r, uint64_t i)
        {
            const size_t compact = find("compact");
            switch (i)
            {
            case 0:
            case 1:
                return standard_case(r, compact, i == 1, NONE);
            case 2:
                return standard_case(r, find("compact_ext"), false, NONE);
            case 3:
            case 4:
            {
                // Every first header pointer edit, one frame after the other
                const Layout &l = LAYOUTS[compact];
                Packets m(r, l, false);
                m.standard();
                Zones zones = m.zones();
                for (uint64_t k = 0; k < 7 && 1 + 2 * k < zones.size(); k++)
                    zones[1 + 2 * k].first = pointer_edit(l, k);
                return make(i == 3 ? "fhp-all-compact" : "fhp-all-compact-strict", l, l.counter, i == 4, false,
                            cadus_of(r, l, zones, 9, counters(r, zones.size(), 0, false), false));
            }
            case 5:
            {
                const Layout &l = LAYOUTS[compact];
                Packets m(r, l, false);
                m.standard();
                Zones zones = m.zones();
                return make("counters-compact", l, true, false, false,
                            cadus_of(r, l, zones, 9, counters(r, zones.size(), 0xFFFFFC, true), true));
            }
            case 6:
                return standard_case(r, compact, true, LIES);
            case 7:
                return standard_case(r, find("tiny"), false, NONE);
            case 8:
            {
                // Three CADUs of MetOp: a packet over all three, a cut header, idle to the end
                const Layout &l = LAYOUTS[find("metop")];
                Packets m(r, l, false);
                m.add(100);
                m.add_until(3);
                m.add(1500);
                Zones zones = m.zones();
                return make("short-metop-strict", l, true, true, false,
                            cadus_of(r, l, zones, 9, counters(r, zones.size(), 0, false), false));
            }
            default:
                return refused_case("refused_5", r, LAYOUTS[find("refused_5")], false, false);
            }
        }
        constexpr uint64_t SMALL_EDGES = 10;

        uint64_t edge_count(const Plan &p)
        {
            return p.small ? SMALL_EDGES : 2 * LAYOUTS.size() + FOCUS.size() * DAMAGES * 2 + MAX_LENGTH.size() * 2;
        }

        Case edge(const Plan &p, uint64_t i)
        {
            Rng r = Rng::for_case(p.seed ^ EDGES, i);
            if (p.small)
                return small_edge(r, i);
            if (i < 2 * LAYOUTS.size())
                return standard_case(r, size_t(i / 2), i % 2 == 1, NONE);
            i -= 2 * LAYOUTS.size();
            if (i < FOCUS.size() * DAMAGES * 2)
                return standard_case(r, FOCUS[i / (DAMAGES * 2)], i % 2 == 1, int((i / 2) % DAMAGES));
            i -= FOCUS.size() * DAMAGES * 2;
            return max_length_case(r, MAX_LENGTH[i / 2], i % 2 == 1);
        }

        Case seeded(const Plan &p, uint64_t i)
        {
            Rng r = Rng::for_case(p.seed ^ SEEDED, i);
            size_t li;
            const uint64_t pick = r.below(20);
            if (p.small)
                li = SMALL_LAYOUTS[r.below(SMALL_LAYOUTS.size())];
            else if (pick < 14)
                li = size_t(r.below(CALL_SITES));
            else if (pick < 19)
                li = CALL_SITES + size_t(r.below(SUPPORTED - CALL_SITES));
            else
                li = SUPPORTED + size_t(r.below(LAYOUTS.size() - SUPPORTED));
            const Layout &l = LAYOUTS[li];
            const bool counter = r.below(4) == 0 ? !l.counter : l.counter;
            const bool strict = r.below(2) == 1;
            const bool route = r.below(5) == 0;
            const std::string id = "seed-" + std::to_string(i) + "-" + l.name;
            if (refused(l))
                return refused_case(id, r, l, counter, strict);

            const bool small_zone = l.mpdu < SMALL_ZONE;
            const bool long_ones = !p.small && !small_zone && r.below(50) == 0;
            size_t target = p.small ? 100 + r.below(500) : long_ones ? 60000 + r.below(20000) : 300 + r.below(24000);
            if (!p.small && small_zone)
                target = size_t(l.mpdu) * (10 + r.below(190)); // 10 to 200 frames (see the top of this file)
            const bool lies = r.below(4) == 0, pointers = r.below(3) == 0, bad_counters = r.below(3) == 0;
            const bool noise = r.below(2) == 0;
            const uint32_t start = r.below(4) == 0 ? uint32_t(0xFFFFFF - r.below(4)) : uint32_t(r.below(1 << 24));

            auto channel = [&](size_t bytes, uint8_t vcid)
            {
                Packets m(r, l, long_ones);
                while (m.size() < bytes)
                    m.add_random(lies);
                Zones zones = m.zones();
                if (pointers)
                    for (auto &z : zones)
                        if (r.below(8) == 0)
                            z.first = r.below(4) == 0 ? uint16_t(r.below(2048)) : pointer_edit(l, r.below(7));
                return cadus_of(r, l, zones, vcid, counters(r, zones.size(), start, bad_counters), noise);
            };
            std::vector<std::vector<uint8_t>> cadus = channel(target, 9);
            if (route)
            {
                // A second virtual channel, its CADUs mixed in among the first one's
                const std::vector<std::vector<uint8_t>> other = channel(target / 3 + 1, 34);
                std::vector<std::vector<uint8_t>> mixed;
                size_t a = 0, b = 0;
                while (a < cadus.size() || b < other.size())
                {
                    const bool first = b == other.size() || (a < cadus.size() && r.below(3) != 0);
                    mixed.push_back(first ? cadus[a++] : other[b++]);
                }
                cadus = mixed;
            }
            Case c = make(id, l, counter, strict, route, cadus);
            if (r.below(4) == 0)
                mutate(c, r, int(1 + r.below(3)));
            return c;
        }
    } // namespace

    const std::vector<Layout> &layouts() { return LAYOUTS; }

    std::string layout_group(const Case &c)
    {
        if (c.params.count("mpdu") == 0)
            return "corpus file (MetOp and Meteor layout)";
        std::string names;
        size_t first = LAYOUTS.size();
        for (size_t i = 0; i < LAYOUTS.size(); i++)
        {
            const Layout &l = LAYOUTS[i];
            if (l.mpdu == c.param("mpdu", 0) && l.has_iz == (c.param("has_iz", 0) != 0) && l.iz == c.param("iz", 0) &&
                l.ext == (c.param("ext", 0) != 0))
            {
                names += (names.empty() ? "" : "/") + std::string(l.name);
                first = std::min(first, i);
            }
        }
        if (names.empty())
            return "other settings";
        return std::string(first < CALL_SITES ? "call site " : first < SUPPORTED ? "other layout " : "refused layout ") + names;
    }

    void set_params(Case &c, const Layout &l, bool counter, bool strict, bool route)
    {
        c.params["mpdu"] = l.mpdu;
        c.params["has_iz"] = l.has_iz;
        c.params["iz"] = l.iz;
        c.params["ext"] = l.ext;
        c.params["counter"] = counter;
        c.params["strict"] = strict;
        c.params["route"] = route;
    }

    uint64_t case_count(const Plan &p) { return edge_count(p) + p.cases; }

    Case make_case(const Plan &p, uint64_t i)
    {
        return i < edge_count(p) ? edge(p, i) : seeded(p, i - edge_count(p));
    }
} // namespace satdump::diff::aos
