// Fork (M6.3): the Meteor LRPT MSU-MR segment decoder as twins (diff.h): the C++ Segment
// (plugins/meteor_support/meteor/instruments/msumr/lrpt/segment.cpp, with huffman.cpp and
// idct.cpp) against the Rust one (rust/src/lrpt), reached as SatDump reaches it, through
// Segment::decodeRust and rust_lrpt.h. The C++ side runs with Rust paused (run.cpp), so it is
// the C++ decoder even though the Segment constructor uses Rust in this build.
//
//   lrpt_segment  one packet payload, with the partial and M2-x flags (params "partial", "m2x";
//                 "length" only for a negative length). Outcome: the 8 x 112 pixels as bytes
//                 and one record with every header field, the time stamp, valid and partial.
//   lrpt_qtable   the quantisation table for quality factor "qf" (0 to 255), as 64 integers.
//
// Corpus files are read as SatDump reads a Meteor .cadu file (VCID 5, the C++ demuxer, APIDs 64
// to 69): every MSU-MR packet is a case, and a copy of it with one bit flipped another. Other
// files are cut into packets as fuzz_msumr_lrpt cuts its input.
#include "common/ccsds/ccsds_aos/demuxer.h"
#include "common/ccsds/ccsds_aos/vcdu.h"
#include "diff.h"
#include "meteor/instruments/msumr/lrpt/segment.h"
#include "meteor/instruments/msumr/lrpt/tables.h"
#include "rust_bridge.h"
#include "rust_lrpt.h"
#include <algorithm>
#include <array>
#include <climits>
#include <cstring>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>

namespace meteor::msumr::lrpt
{
    // As huffman.h declares it. That header also defines EOB and CFC, statics this file has no
    // use for (GCC would warn that they are unused here).
    std::array<int64_t, 64> GetQuantizationTable(float qf);
} // namespace meteor::msumr::lrpt

namespace satdump::diff
{
    namespace
    {
        namespace lrpt = meteor::msumr::lrpt;
        using lrpt::Segment;

        constexpr size_t HEADER = 14;
        constexpr uint64_t DECISIONS = 0x6C727074ULL; // "lrpt": a generator of its own for the choices
        constexpr uint64_t BASES = 0x62617365ULL;     // and one for the packets cut or flipped everywhere

        // ---- What both sides give

        Outcome outcome_of(const Segment &s, int64_t status)
        {
            Outcome o;
            o.status = status;
            std::vector<uint8_t> pixels(sizeof(s.lines));
            std::memcpy(pixels.data(), s.lines, pixels.size());
            o.bytes = Value::data(std::move(pixels));
            o.records = {{{"day_time", Value::integer(s.day_time)},
                          {"ms_time", Value::integer(s.ms_time)},
                          {"us_time", Value::integer(s.us_time)},
                          {"timestamp", Value::doubles({s.timestamp})},
                          {"mcun", Value::integer(s.MCUN)},
                          {"qt", Value::integer(s.QT)},
                          {"dc", Value::integer(s.DC)},
                          {"ac", Value::integer(s.AC)},
                          {"qfm", Value::integer(s.QFM)},
                          {"qf", Value::integer(s.QF)},
                          {"valid", Value::integer(s.valid)},
                          {"partial", Value::integer(s.partial)}}};
            return o;
        }

        // The length both sides are told: the input's, unless the case gives a negative one
        int length_of(const Case &c)
        {
            const int64_t real = int64_t(c.input.size());
            const int64_t length = c.param("length", real);
            if (length >= 0 && length != real)
                throw std::invalid_argument("case " + c.id + ": a length that is not the input's");
            return int(length);
        }

        Outcome segment_cpp(const Case &c)
        {
            std::vector<uint8_t> buf = c.input;
            const int length = length_of(c);
            const Segment s(buf.data(), length, c.param("partial", 0) != 0, c.param("m2x", 0) != 0);
            // The C++ code has no status. The Rust rules (rust/src/ffi.rs) refuse a negative length,
            // and the C++ code then decodes the packet: an invalid segment.
            return outcome_of(s, length < 0 ? satdump::rust::STATUS_BAD_ARGUMENT : satdump::rust::STATUS_OK);
        }

        Outcome segment_rust(const Case &c)
        {
            std::vector<uint8_t> buf = c.input;
            const int length = length_of(c);
            const bool partial = c.param("partial", 0) != 0, m2x = c.param("m2x", 0) != 0;
            Segment s;
            if (s.decodeRust(buf.data(), length, partial, m2x))
                return outcome_of(s, satdump::rust::STATUS_OK);
            // Rust did not do it. The status says why (a failure, a panic, switches Rust off), and
            // the segment is what SatDump then has: the C++ decode of the same packet.
            const int64_t status = satdump::rust::enabled() ? satdump::rust::STATUS_BAD_ARGUMENT : satdump::rust::STATUS_PANIC;
            return outcome_of(Segment(buf.data(), length, partial, m2x), status);
        }

        Outcome qtable_cpp(const Case &c)
        {
            const std::array<int64_t, 64> t = lrpt::GetQuantizationTable(float(c.param("qf", 0)));
            Outcome o;
            o.bytes = Value::data(std::vector<uint8_t>((const uint8_t *)t.data(), (const uint8_t *)t.data() + sizeof(t)));
            o.records = {{{"qf", Value::integer(c.param("qf", 0))}}};
            return o;
        }

        Outcome qtable_rust(const Case &c)
        {
            int64_t t[64] = {0};
            Outcome o;
            if (!satdump::rust::lrpt_quant_table(int(c.param("qf", 0)), t))
                o.status = satdump::rust::enabled() ? satdump::rust::STATUS_BAD_ARGUMENT : satdump::rust::STATUS_PANIC;
            o.bytes = Value::data(std::vector<uint8_t>((const uint8_t *)t, (const uint8_t *)t + sizeof(t)));
            o.records = {{{"qf", Value::integer(c.param("qf", 0))}}};
            return o;
        }

        // ---- Packets made the way the satellite codes them, with the decoder's own tables

        class BitWriter
        {
        public:
            std::vector<uint8_t> bytes;

            void bit(bool b)
            {
                if (n % 8 == 0)
                    bytes.push_back(0);
                if (b)
                    bytes.back() |= uint8_t(0x80 >> (n % 8));
                n++;
            }

            void code(const bool *c, int length)
            {
                for (int i = 0; i < length; i++)
                    bit(c[i]);
            }

            // A value in `size` bits, a negative one less 1, as getValue reads it
            void value(int64_t v, int size)
            {
                const uint64_t raw = uint64_t(v >= 0 ? v : v + (int64_t(1) << size) - 1);
                for (int i = size - 1; i >= 0; i--)
                    bit((raw >> i) & 1);
            }

            // To a whole byte
            void pad(bool ones)
            {
                while (n % 8)
                    bit(ones);
            }

        private:
            size_t n = 0;
        };

        // A value of `size` bits (1 << (size - 1) up to (1 << size) - 1, either sign); 0 for size 0
        int64_t value_of_size(Rng &r, int size)
        {
            if (size == 0)
                return 0;
            const int64_t low = int64_t(1) << (size - 1);
            const int64_t v = low + int64_t(r.below(uint64_t(low)));
            return r.below(2) ? v : -v;
        }

        const lrpt::dc_t &dc_code(int size)
        {
            for (const lrpt::dc_t &m : lrpt::dcCategories)
                if (m.len == size)
                    return m;
            throw std::logic_error("no DC code for that size");
        }

        const lrpt::ac_t &ac_code(int zeros, int size)
        {
            for (const lrpt::ac_t &m : lrpt::acCategories)
                if (m.zlen == zeros && m.clen == size)
                    return m;
            throw std::logic_error("no AC code for that run and size");
        }

        // One block: its DC difference, then AC coefficients, each after a run of zeros. Detail 0
        // is a flat block, 1 a few coefficients, 2 many, 3 a block coded up to its end. The
        // decoder stops at the end of block, or at a coefficient that would reach the block's
        // 64th place (it reads it and does not store it), so that one is the last one written.
        void write_block(BitWriter &w, Rng &r, int detail)
        {
            // Mostly small DC differences, now and then up to the largest (11 bits)
            const int dc_size = int(r.below(4) == 0 ? r.below(12) : r.below(5));
            const lrpt::dc_t &dc = dc_code(dc_size);
            w.code(dc.code, dc.klen);
            w.value(value_of_size(r, dc_size), dc_size);

            const int wanted = detail == 0 ? 0 : detail == 1 ? 1 + int(r.below(5)) : detail == 2 ? 5 + int(r.below(25)) : 64;
            int index = 1; // the next place in the block
            for (int k = 0; k < wanted; k++)
            {
                if (r.below(12) == 0)
                {
                    // Sixteen zeros at once
                    const lrpt::ac_t &zrl = ac_code(15, 0);
                    w.code(zrl.code, zrl.klen);
                    if ((index += 16) >= 64)
                        return;
                    continue;
                }
                const int zeros = detail == 3 ? int(r.below(2)) : int(r.below(r.below(4) == 0 ? 16 : 3));
                const int size = 1 + int(r.below(r.below(3) == 0 ? 10 : 4));
                const lrpt::ac_t &m = ac_code(zeros, size);
                w.code(m.code, m.klen);
                w.value(value_of_size(r, size), size);
                if ((index += zeros + 1) >= 64)
                    return;
            }
            const lrpt::ac_t &eob = ac_code(0, 0);
            w.code(eob.code, eob.klen);
        }

        struct Header
        {
            uint16_t day = 9080;
            uint32_t ms = 43200000;
            uint16_t us = 0;
            uint8_t mcun = 0, qt = 0, dcac = 0;
            uint16_t qfm = 0xFFF0;
            uint8_t qf = 80;
        };

        // A header as the satellite sends one, at a random time, now and then any 8 bytes of time
        Header random_header(Rng &r)
        {
            Header h;
            h.day = uint16_t(r.below(4) == 0 ? r.next() : 9000 + r.below(400));
            h.ms = uint32_t(r.below(4) == 0 ? r.next() : r.below(86400000));
            h.us = uint16_t(r.below(4) == 0 ? r.next() : r.below(1000));
            h.mcun = uint8_t(14 * r.below(14));
            h.qf = uint8_t(r.below(3) == 0 ? r.next() : 70 + r.below(30));
            return h;
        }

        // The header, `blocks` coded blocks of the given detail (-1: each one picked at random,
        // `max_detail` at most), then bits of 1 (or 0) to a whole byte
        std::vector<uint8_t> packet(Rng &r, const Header &h, int blocks, int detail, int max_detail = 3, bool pad_ones = true)
        {
            BitWriter w;
            for (int b = 0; b < blocks; b++)
                write_block(w, r, detail >= 0 ? detail : int(r.below(uint64_t(max_detail) + 1)));
            w.pad(pad_ones);
            std::vector<uint8_t> out = {uint8_t(h.day >> 8), uint8_t(h.day), uint8_t(h.ms >> 24), uint8_t(h.ms >> 16),
                                        uint8_t(h.ms >> 8), uint8_t(h.ms), uint8_t(h.us >> 8), uint8_t(h.us), h.mcun, h.qt,
                                        h.dcac, uint8_t(h.qfm >> 8), uint8_t(h.qfm), h.qf};
            out.insert(out.end(), w.bytes.begin(), w.bytes.end());
            return out;
        }

        // A case that carries its bytes (the Rust tests have no packet coder)
        Case structured(const std::string &id, const Plan &p, uint64_t index, std::vector<uint8_t> bytes, bool partial, bool m2x)
        {
            Case c;
            c.id = id;
            c.seed = p.seed;
            c.index = index;
            c.input = std::move(bytes);
            c.length = c.input.size();
            c.inline_input = true;
            c.params["partial"] = partial;
            c.params["m2x"] = m2x;
            return c;
        }

        // The good packets cut at every length (truncated) and with every bit flipped (flipped)
        std::vector<uint8_t> base_packet(const Plan &p, uint64_t k, bool flipped)
        {
            Rng r = Rng::for_case(p.seed ^ BASES ^ (flipped ? 1 : 0), k);
            return packet(r, random_header(r), 14, -1, flipped ? 2 : 3);
        }

        // ---- The plan, in parts. A small plan (committed oracles) takes a few of each.

        const std::vector<int64_t> NEGATIVE = {-1, -14, -15, -100000, int64_t(INT32_MIN) + 14};
        const std::vector<size_t> SMALL_EDGES = {0, 13, 14, 15};
        const std::vector<uint64_t> SMALL_QF = {0, 20, 49, 80, 255};
        const std::vector<uint64_t> SMALL_BLOCKS = {0, 9};
        // The edges of the table formula's two branches, and 80 (the usual one)
        const std::vector<uint64_t> SMALL_QTABLE = {0, 19, 20, 21, 49, 50, 80};
        constexpr uint64_t EDGE_LENGTHS = 17; // 0..16 bytes, each partial or not, M2-x or not
        constexpr uint64_t TRUNCATED = 3, FLIPPED = 2;

        // The sizes of those packets, worked out once per seed (every case of the plan needs them)
        const std::vector<uint64_t> &base_sizes(const Plan &p, bool flipped)
        {
            static std::mutex lock;
            static std::map<std::pair<uint64_t, bool>, std::vector<uint64_t>> sizes;
            const std::lock_guard<std::mutex> hold(lock);
            std::vector<uint64_t> &v = sizes[{p.seed, flipped}];
            if (v.empty())
                for (uint64_t k = 0; k < (flipped ? FLIPPED : TRUNCATED); k++)
                    v.push_back(base_packet(p, k, flipped).size());
            return v;
        }

        // Cases for each packet: every length 0 to its size, or every bit
        uint64_t truncated_count(const Plan &p, uint64_t k) { return base_sizes(p, false).at(k) + 1; }
        uint64_t flipped_count(const Plan &p, uint64_t k) { return base_sizes(p, true).at(k) * 8; }

        struct Part
        {
            const char *name;
            uint64_t (*count)(const Plan &);
            Case (*make)(const Plan &, uint64_t i, uint64_t index);
        };

        const Part PARTS[] = {
            // Empty, header-only and nearly header-only payloads
            {"edges", [](const Plan &p) { return p.small ? uint64_t(SMALL_EDGES.size()) : EDGE_LENGTHS * 4; },
             [](const Plan &p, uint64_t i, uint64_t index)
             {
                 const size_t length = p.small ? SMALL_EDGES[i] : size_t(i / 4);
                 const uint64_t flags = p.small ? i : i % 4;
                 Case c = generated("edge-" + std::to_string(length) + "-flags" + std::to_string(flags), p.seed, index, length, Style::Random);
                 c.params["partial"] = int64_t(flags & 1);
                 c.params["m2x"] = int64_t(flags >> 1 & 1);
                 return c;
             }},
            // Negative lengths: Rust refuses them, the C++ decodes an invalid segment
            {"negative", [](const Plan &p) { return p.small ? uint64_t(1) : uint64_t(NEGATIVE.size()); },
             [](const Plan &p, uint64_t i, uint64_t index)
             {
                 Case c = generated("negative" + std::to_string(NEGATIVE[i]), p.seed, index, 32, Style::Random);
                 c.params["length"] = NEGATIVE[i];
                 return c;
             }},
            // Every quality factor, in good packets
            {"qf", [](const Plan &p) { return p.small ? uint64_t(SMALL_QF.size()) : uint64_t(256); },
             [](const Plan &p, uint64_t i, uint64_t index)
             {
                 Rng r = Rng::for_case(p.seed ^ DECISIONS, index);
                 Header h = random_header(r);
                 h.qf = uint8_t(p.small ? SMALL_QF[i] : i);
                 const bool m2x = r.below(2);
                 return structured("qf-" + std::to_string(h.qf), p, index, packet(r, h, 14, -1, p.small ? 1 : 3), false, m2x);
             }},
            // Every MCU number, other table numbers and quality markers, and the extreme times
            {"header", [](const Plan &p) { return p.small ? uint64_t(1) : uint64_t(256); },
             [](const Plan &p, uint64_t i, uint64_t index)
             {
                 Rng r = Rng::for_case(p.seed ^ DECISIONS, index);
                 Header h = random_header(r);
                 h.mcun = uint8_t(i);
                 if (i % 4 != 0)
                     h.qt = uint8_t(r.next()), h.dcac = uint8_t(r.next()), h.qfm = uint16_t(r.next());
                 if (i < 8)
                 {
                     h.day = i & 1 ? 0xFFFF : 0;
                     h.ms = i & 2 ? 0xFFFFFFFF : 0;
                     h.us = i & 4 ? 0xFFFF : 0;
                 }
                 return structured("header-" + std::to_string(i), p, index, packet(r, h, 14, -1, 1), false, i & 1);
             }},
            // Picture data that ends after 0 to 14 whole blocks, padded with bits of 1 or of 0
            {"blocks", [](const Plan &p) { return p.small ? uint64_t(SMALL_BLOCKS.size()) : uint64_t(15 * 4); },
             [](const Plan &p, uint64_t i, uint64_t index)
             {
                 Rng r = Rng::for_case(p.seed ^ DECISIONS, index);
                 const int blocks = int(p.small ? SMALL_BLOCKS[i] : i / 4);
                 const bool ones = p.small || i % 2 == 0, partial = !p.small && (i / 2) % 2 == 1;
                 return structured("blocks-" + std::to_string(blocks) + (ones ? "-ones" : "-zeros") + (partial ? "-partial" : ""), p,
                                   index, packet(r, Header(), blocks, -1, p.small ? 1 : 3, ones), partial, true);
             }},
            // Good packets cut at every length
            {"truncated",
             [](const Plan &p)
             {
                 if (p.small)
                     return uint64_t(1);
                 uint64_t n = 0;
                 for (uint64_t k = 0; k < TRUNCATED; k++)
                     n += truncated_count(p, k);
                 return n;
             },
             [](const Plan &p, uint64_t i, uint64_t index)
             {
                 uint64_t k = 0;
                 while (!p.small && i >= truncated_count(p, k))
                     i -= truncated_count(p, k++);
                 const std::vector<uint8_t> full = base_packet(p, k, false);
                 const uint64_t length = p.small ? full.size() * 2 / 3 : i;
                 Case c = structured("truncated-" + std::to_string(k) + "-" + std::to_string(length), p, index, full, length < full.size(), true);
                 truncate(c, length);
                 return c;
             }},
            // Good packets with every bit flipped in turn (Huffman codes and header alike)
            {"flipped",
             [](const Plan &p)
             {
                 if (p.small)
                     return uint64_t(1);
                 uint64_t n = 0;
                 for (uint64_t k = 0; k < FLIPPED; k++)
                     n += flipped_count(p, k);
                 return n;
             },
             [](const Plan &p, uint64_t i, uint64_t index)
             {
                 uint64_t k = 0;
                 while (!p.small && i >= flipped_count(p, k))
                     i -= flipped_count(p, k++);
                 const std::vector<uint8_t> full = base_packet(p, k, true);
                 const uint64_t bit = p.small ? HEADER * 8 + 3 : i;
                 Case c = structured("flipped-" + std::to_string(k) + "-bit" + std::to_string(bit), p, index, full, false, true);
                 flip_bit(c, bit);
                 return c;
             }},
            // Seeded: any bytes (the fuzz harness's packets), good packets, and good packets
            // damaged or cut short
            {"seeded", [](const Plan &p) { return p.cases; },
             [](const Plan &p, uint64_t i, uint64_t index)
             {
                 Rng r = Rng::for_case(p.seed ^ DECISIONS, index);
                 const std::string id = "seed-" + std::to_string(i);
                 const uint64_t kind = r.below(4);
                 if (kind == 0)
                 {
                     const size_t length = pick_length(r, p.small ? 64 : 2048, {HEADER, HEADER + 1, 64, 882});
                     Case c = generated(id, p.seed, index, length, Style(r.below(uint64_t(Style::Count))));
                     c.params["partial"] = int64_t(r.below(2));
                     c.params["m2x"] = int64_t(r.below(2));
                     return c;
                 }
                 const bool m2x = r.below(2);
                 Case c = structured(id, p, index, packet(r, random_header(r), 14, -1, p.small ? 1 : 3), false, m2x);
                 if (kind == 2)
                     mutate(c, r, 1 + int(r.below(4)));
                 else if (kind == 3)
                 {
                     truncate(c, r.below(c.input.size() + 1));
                     c.params["partial"] = 1;
                 }
                 return c;
             }},
        };

        uint64_t segment_count(const Plan &p)
        {
            uint64_t n = 0;
            for (const Part &part : PARTS)
                n += part.count(p);
            return n;
        }

        Case segment_make(const Plan &p, uint64_t index)
        {
            uint64_t i = index;
            for (const Part &part : PARTS)
            {
                const uint64_t n = part.count(p);
                if (i < n)
                    return part.make(p, i, index);
                i -= n;
            }
            throw std::out_of_range("lrpt_segment has no case " + std::to_string(index));
        }

        // ---- Corpus files

        // A copy of a packet case with one bit flipped, where the packet's own digest says
        void add_with_flip(std::vector<Case> &out, Case c)
        {
            out.push_back(c);
            if (c.input.empty())
                return;
            c.id += "-flip";
            flip_bit(c, fnv1a(c.input.data(), c.input.size()) % (c.input.size() * 8));
            out.push_back(c);
        }

        std::vector<Case> corpus_segments(const std::string &name, const std::vector<uint8_t> &data)
        {
            std::vector<Case> out;
            auto add = [&](const std::string &id, std::vector<uint8_t> payload, bool partial, bool m2x)
            {
                Case c;
                c.id = name + "-" + id;
                c.input = std::move(payload);
                c.length = c.input.size();
                c.inline_input = true;
                c.params["partial"] = partial;
                c.params["m2x"] = m2x;
                add_with_flip(out, c);
            };

            static const uint8_t SYNC[4] = {0x1A, 0xCF, 0xFC, 0x1D};
            if (data.size() >= 1024 && data.size() % 1024 == 0 && std::memcmp(data.data(), SYNC, 4) == 0)
            {
                // CADUs, read as module_meteor_msumr_lrpt.cpp reads them. The demuxer stays the C++
                // one, so these packets are the same whichever parts of SatDump use Rust.
                const satdump::rust::CppOnly paused;
                ccsds::ccsds_aos::Demuxer demuxer(882, true);
                std::vector<uint8_t> cadu(1024);
                size_t frames = 0, n = 0;
                for (size_t at = 0; at < data.size(); at += 1024)
                {
                    std::memcpy(cadu.data(), data.data() + at, 1024);
                    if (ccsds::ccsds_aos::parseVCDU(cadu.data()).vcid != 5)
                        continue;
                    frames++;
                    for (ccsds::CCSDSPacket &pkt : demuxer.work(cadu.data()))
                    {
                        if (pkt.header.apid < 64 || pkt.header.apid > 69)
                            continue;
                        // The partial flag as lrpt_msumr_reader.cpp works it out. The golden
                        // recording is Meteor-M N2-3, decoded in M2-x mode.
                        const bool partial = pkt.payload.size() - 1 != pkt.header.packet_length;
                        add("packet" + std::to_string(n++) + "-apid" + std::to_string(pkt.header.apid), pkt.payload, partial, true);
                    }
                }
                // Virtual channel 5 with no MSU-MR packet in it: the corpus would check nothing
                if (frames > 0 && n == 0)
                    throw std::runtime_error(name + ": virtual channel 5 holds no MSU-MR packets");
                return out;
            }

            // Any other file: packets as fuzz_msumr_lrpt makes them, [APID offset] [sequence, 2]
            // [length, 2] [flags] [payload]; flags bit 0 M2-x (of the first packet), bit 1 cut short
            const bool m2x = data.size() > 5 && (data[5] & 1);
            size_t pos = 0, n = 0;
            while (pos + 6 <= data.size())
            {
                const size_t want = size_t(data[pos + 3] << 8 | data[pos + 4]) % 2048;
                const bool cut = data[pos + 5] & 2;
                pos += 6;
                const size_t len = std::min(want, data.size() - pos);
                // packet_length as the harness sets it, and the flag as the reader works it out
                const size_t packet_length = cut ? len + 7 : (len ? len - 1 : 0);
                const bool partial = len - 1 != packet_length;
                add("fuzz" + std::to_string(n++), std::vector<uint8_t>(data.begin() + pos, data.begin() + pos + len), partial, m2x);
                pos += len;
            }
            return out;
        }
    } // namespace

    void add_lrpt_twins(std::vector<Twin> &twins)
    {
        Twin s;
        s.name = "lrpt_segment";
        s.about = "Meteor LRPT MSU-MR segment decoder (meteor::msumr::lrpt::Segment)";
        s.cpp = segment_cpp;
        s.rust = segment_rust;
        s.count = segment_count;
        s.make = segment_make;
        // Corpus files are read as CADUs (corpus_reader); the frame size only marks the twin as one
        // that reads them
        s.corpus_frame = 1024;
        s.corpus_reader = corpus_segments;
        twins.push_back(s);

        Twin q;
        q.name = "lrpt_qtable";
        q.about = "Meteor LRPT quantisation table for each quality factor (GetQuantizationTable)";
        q.cpp = qtable_cpp;
        q.rust = qtable_rust;
        q.count = [](const Plan &p) { return p.small ? uint64_t(SMALL_QTABLE.size()) : uint64_t(256); };
        q.make = [](const Plan &p, uint64_t index)
        {
            const uint64_t qf = p.small ? SMALL_QTABLE.at(index) : index;
            Case c = generated("qf-" + std::to_string(qf), p.seed, index, 0, Style::Zero);
            c.params["qf"] = int64_t(qf);
            return c;
        };
        twins.push_back(q);
    }
} // namespace satdump::diff
