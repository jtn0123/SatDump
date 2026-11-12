// Fork: the CCSDS derandomisers (src-core/common/codings/randomization.cpp) as twins: the C++
// derand_ccsds_cpp, derand_ccsds_soft and derand_ccsds_bits against the Rust ones, called
// through rust_bridge.h as SatDump calls them (SatDump uses only the first one from Rust so far).
// The C++ side runs with Rust paused (run.cpp), so if derand_ccsds_soft or derand_ccsds_bits
// are later made to call Rust, as derand_ccsds does, these twins still compare Rust with the
// C++ code and not with itself (test_diff_rules.cpp checks the pause).
#include "common/codings/randomization.h"
#include "diff.h"
#include "rust_bridge.h"
#include <climits>
#include <string>

namespace satdump::diff
{
    namespace
    {
        constexpr uint8_t BEFORE = 0xA5, AFTER = 0x5A;
        constexpr size_t GUARD = 8;

        // The buffer both sides get: `offset` bytes of 0xA5 (so the data is not aligned), the
        // input, then 8 bytes of 0x5A. The outcome is the whole buffer, so a write outside the
        // input shows up as well.
        std::vector<uint8_t> layout(const Case &c, size_t &offset)
        {
            offset = size_t(c.param("offset", 0));
            std::vector<uint8_t> buf(offset, BEFORE);
            buf.insert(buf.end(), c.input.begin(), c.input.end());
            buf.insert(buf.end(), GUARD, AFTER);
            return buf;
        }

        // The length both sides are told: the input's, unless the case gives another (a negative one)
        int length_of(const Case &c)
        {
            return int(c.param("length", int64_t(c.input.size())));
        }

        template <typename T>
        Runner cpp_side(void (*fn)(T *, int))
        {
            return [fn](const Case &c)
            {
                size_t offset;
                std::vector<uint8_t> buf = layout(c, offset);
                const int length = length_of(c);
                Outcome o;
                // The C++ code has no status. The Rust rules (rust/src/ffi.rs) refuse a negative
                // length and change nothing, which is what the C++ loop does with one.
                if (length < 0)
                    o.status = satdump::rust::STATUS_BAD_ARGUMENT;
                else
                    fn(reinterpret_cast<T *>(buf.data() + offset), length);
                o.bytes = Value::data(std::move(buf));
                return o;
            };
        }

        template <typename T>
        Runner rust_side(bool (*fn)(T *, int))
        {
            return [fn](const Case &c)
            {
                size_t offset;
                std::vector<uint8_t> buf = layout(c, offset);
                Outcome o;
                // The bridge says only whether Rust did the work; after a failure (a panic) it
                // switches Rust off, which tells the two apart
                if (!fn(reinterpret_cast<T *>(buf.data() + offset), length_of(c)))
                    o.status = satdump::rust::enabled() ? satdump::rust::STATUS_BAD_ARGUMENT : satdump::rust::STATUS_PANIC;
                o.bytes = Value::data(std::move(buf));
                return o;
            };
        }

        // Around the sequence's period (255), a CADU (1020 bytes after the sync marker), powers of two
        const std::vector<size_t> EDGES = {0, 1, 2, 7, 8, 9, 31, 32, 33, 254, 255, 256, 257, 509, 510, 511, 1019,
                                           1020, 1021, 1023, 1024, 1025, 2040, 4095, 4096, 8191, 8192, 8193, 65535, 65536};
        const std::vector<size_t> SMALL_EDGES = {0, 1, 2, 7, 8, 9, 254, 255, 256, 257, 510, 1020};
        const std::vector<int64_t> NEGATIVE = {-1, -2, -255, -4096, INT32_MIN};
        constexpr uint64_t SWEEP = 4097;                      // every length 0..4096
        constexpr uint64_t STYLES = uint64_t(Style::Count);   // each edge length in every fill style...
        constexpr uint64_t OFFSETS = 2;                       // ...at offsets 0 and 3
        constexpr uint64_t DECISIONS = 0x64657261ULL;         // a generator of its own for the choices

        uint64_t edge_count(const Plan &p)
        {
            return p.small ? SMALL_EDGES.size() : EDGES.size() * STYLES * OFFSETS;
        }

        // A normal plan: every length 0..4096 (offset length % 8), the edge lengths, negative
        // lengths, then `cases` seeded ones up to 70000 bytes. A small plan (committed oracles):
        // fewer edge lengths and seeded inputs up to 256 bytes.
        uint64_t derand_count(const Plan &p)
        {
            return (p.small ? 0 : SWEEP) + edge_count(p) + NEGATIVE.size() + p.cases;
        }

        Case derand_make(const Plan &p, uint64_t index)
        {
            uint64_t i = index;
            if (!p.small)
            {
                if (i < SWEEP)
                {
                    Case c = generated("len-" + std::to_string(i), p.seed, index, size_t(i), Style::Random);
                    c.params["offset"] = int64_t(i % 8);
                    return c;
                }
                i -= SWEEP;
            }
            if (i < edge_count(p))
            {
                const size_t length = p.small ? SMALL_EDGES[i] : EDGES[i / (STYLES * OFFSETS)];
                const Style style = p.small ? Style::Random : Style((i / OFFSETS) % STYLES);
                const int64_t offset = p.small ? int64_t(i % 4) : int64_t(i % OFFSETS) * 3;
                Case c = generated("edge-" + std::to_string(length) + "-style" + std::to_string(int(style)) + "-at" +
                                       std::to_string(offset),
                                   p.seed, index, length, style);
                c.params["offset"] = offset;
                return c;
            }
            i -= edge_count(p);
            if (i < NEGATIVE.size())
            {
                Case c = generated("negative" + std::to_string(NEGATIVE[i]), p.seed, index, 16, Style::Random);
                c.params["length"] = NEGATIVE[i];
                c.params["offset"] = 1;
                return c;
            }
            i -= NEGATIVE.size();
            Rng r = Rng::for_case(p.seed ^ DECISIONS, index);
            const size_t length = pick_length(r, p.small ? 256 : 70000, {255, 1020, 1024, 2040});
            Case c = generated("seed-" + std::to_string(i), p.seed, index, length, Style(r.below(STYLES)));
            c.params["offset"] = int64_t(r.below(8));
            return c;
        }

        Twin derand_twin(const std::string &name, const std::string &about, Runner cpp, Runner rust)
        {
            Twin t;
            t.name = name;
            t.about = about;
            t.cpp = std::move(cpp);
            t.rust = std::move(rust);
            t.count = derand_count;
            t.make = derand_make;
            // Corpus files are read as CADUs, derandomised from byte 4 as the decoders do
            t.corpus_frame = 1024;
            t.corpus_skip = 4;
            return t;
        }
    } // namespace

    void add_derand_twins(std::vector<Twin> &twins)
    {
        twins.push_back(derand_twin("derand_ccsds", "CCSDS derandomiser on bytes (derand_ccsds_cpp)",
                                    cpp_side<uint8_t>(derand_ccsds_cpp), rust_side<uint8_t>(satdump::rust::derand_ccsds)));
        twins.push_back(derand_twin("derand_ccsds_soft", "CCSDS derandomiser on soft symbols (derand_ccsds_soft)",
                                    cpp_side<int8_t>(derand_ccsds_soft), rust_side<int8_t>(satdump::rust::derand_ccsds_soft)));
        twins.push_back(derand_twin("derand_ccsds_bits", "CCSDS derandomiser on one bit per byte (derand_ccsds_bits)",
                                    cpp_side<uint8_t>(derand_ccsds_bits), rust_side<uint8_t>(satdump::rust::derand_ccsds_bits)));
    }

    Twin derand_with_flipped_bit(size_t length, size_t byte)
    {
        Twin t = *find_twin("derand_ccsds");
        const Runner real = t.rust;
        t.name = "derand_ccsds_flipped";
        t.rust = [real, length, byte](const Case &c)
        {
            Outcome o = real(c);
            if (c.input.size() == length && length_of(c) >= 0)
                o.bytes.bytes.at(size_t(c.param("offset", 0)) + byte) ^= 0x01;
            return o;
        };
        return t;
    }
} // namespace satdump::diff
