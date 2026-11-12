// Fork: the framework's own twins (diff.h). Until a stateful decoder is ported they stand in
// for one, so the parts the AOS demuxer and the LRPT decoder will need (chunks, records,
// floats, structured cases, oracles the Rust tests replay) are tested now:
//   selftest_splitter           a toy stream format, fed in chunks, giving records: a reference
//                               that reads one byte at a time against one that reads whole
//                               records; rust/tests/difftest/toys.rs replays its oracle
//   selftest_lowpass            a float filter fed in chunks, C++ only, built as the float
//                               rule's default says (floats.cpp); the Rust version replays its
//                               oracle bit for bit
//   selftest_lowpass_contracted the same filter built with the project's own flags, within a
//                               stated ULP tolerance of the other: the tolerance path (rule 3)
//   selftest_gapfill            the Meteor LRPT line repair, pixels (integers) out of floats,
//                               C++ only, built as rule 1 says; the Rust version replays its
//                               oracle exactly. Its edge cases carry their bytes (inline_input).
#include "diff.h"
#include <string>
#include <utility>

namespace satdump::diff
{
    namespace
    {
        // Records of [tag][length][length bytes]; state carries over from one chunk to the next
        Record split_record(uint64_t at, uint8_t tag, std::vector<uint8_t> payload)
        {
            return {{"at", Value::integer(int64_t(at))}, {"tag", Value::integer(tag)}, {"payload", Value::data(std::move(payload))}};
        }

        // The reference: one byte at a time
        class SplitByByte
        {
        public:
            void feed(const uint8_t *d, size_t n, std::vector<Record> &out)
            {
                for (size_t i = 0; i < n; i++, pos++)
                {
                    if (have == 0)
                        at = pos, tag = d[i], have = 1, payload.clear();
                    else if (have == 1)
                        length = d[i], have = 2;
                    else
                        payload.push_back(d[i]);
                    if (have == 2 && payload.size() == length)
                        out.push_back(split_record(at, tag, payload)), have = 0;
                }
            }
            int64_t left() const { return have == 0 ? 0 : int64_t(have + payload.size()); }

        private:
            uint64_t pos = 0, at = 0;
            uint8_t tag = 0;
            size_t length = 0;
            int have = 0; // header bytes read
            std::vector<uint8_t> payload;
        };

        // The second implementation: whole records out of what has arrived so far
        class SplitByRecord
        {
        public:
            explicit SplitByRecord(bool boundary_bug) : bug(boundary_bug) {}
            void feed(const uint8_t *d, size_t n, std::vector<Record> &out)
            {
                // The deliberate bug: a header cut after its first byte is forgotten
                if (bug && pending.size() == 1)
                    pending.clear(), start++;
                pending.insert(pending.end(), d, d + n);
                size_t p = 0;
                while (pending.size() - p >= 2 && pending.size() - p >= 2 + size_t(pending[p + 1]))
                {
                    const size_t len = pending[p + 1];
                    out.push_back(split_record(start + p, pending[p],
                                               std::vector<uint8_t>(pending.begin() + p + 2, pending.begin() + p + 2 + len)));
                    p += 2 + len;
                }
                pending.erase(pending.begin(), pending.begin() + p);
                start += p;
            }
            int64_t left() const { return int64_t(pending.size()); }

        private:
            bool bug;
            std::vector<uint8_t> pending;
            uint64_t start = 0;
        };

        template <typename S>
        Outcome split(const Case &c, S s)
        {
            Outcome o;
            size_t pos = 0;
            for (size_t n : chunks_of(c))
            {
                s.feed(c.input.data() + pos, n, o.records);
                pos += n;
            }
            o.status = s.left();
            return o;
        }

        // Chunks of every kind: none, one byte each, random with empty ones, fixed; some mutated
        Case stream_case(const Plan &p, uint64_t i, uint64_t salt, size_t max_length)
        {
            Rng r = Rng::for_case(p.seed ^ salt, i);
            const size_t length = pick_length(r, max_length, {2, 258});
            Case c = generated("seed-" + std::to_string(i), p.seed, i, length, i % 3 == 2 ? Style::Sparse : Style::Random);
            switch (i % 4)
            {
            case 0:
                break;
            case 1:
                c.chunks = fixed_chunks(length, 1);
                break;
            case 2:
                c.chunks = random_chunks(r, length, 300, true);
                break;
            default:
                c.chunks = fixed_chunks(length, size_t(1 + r.below(64)));
                break;
            }
            if (i % 5 == 4)
                mutate(c, r, int(1 + r.below(3)));
            return c;
        }

        Outcome lowpass(const Case &c, float (*fn)(float, const uint8_t *, size_t, float *))
        {
            Outcome o;
            float y = 0;
            size_t pos = 0;
            const std::vector<size_t> chunks = chunks_of(c);
            for (size_t k = 0; k < chunks.size(); k++)
            {
                std::vector<float> out(chunks[k]);
                y = fn(y, c.input.data() + pos, chunks[k], out.data());
                pos += chunks[k];
                o.records.push_back({{"chunk", Value::integer(int64_t(k))}, {"y", Value::floats(out)}});
            }
            return o;
        }

        // Each gap is 3 bytes: the good pixel above, the one below, and how far apart they are.
        // Record k is the gap of bytes 3k..3k+2.
        Outcome gapfill(const Case &c, void (*fn)(uint8_t, uint8_t, size_t, uint8_t *))
        {
            Outcome o;
            for (size_t i = 0; i + 3 <= c.input.size(); i += 3)
            {
                const size_t range = 2 + c.input[i + 2] % 49; // 2..50 lines (max_fill_lines is 50 by default)
                std::vector<uint8_t> pixels(range - 1);
                fn(c.input[i], c.input[i + 1], range, pixels.data());
                o.records.push_back({{"range", Value::integer(int64_t(range))}, {"pixels", Value::data(std::move(pixels))}});
            }
            return o;
        }

        // The first three are in the small plan (the committed oracle)
        const std::vector<std::pair<uint8_t, uint8_t>> GAP_PAIRS = {{7, 1}, {0, 255}, {255, 255}, {1, 7}, {255, 0}, {128, 127}};

        uint64_t gap_pairs(const Plan &p)
        {
            return p.small ? 3 : GAP_PAIRS.size();
        }

        // Every range for each pair of pixel values, then seeded gaps
        Case gapfill_case(const Plan &p, uint64_t i)
        {
            if (i < gap_pairs(p))
            {
                // Made by a generator of its own, as a structured one (CADUs packed with packets)
                // would be: the oracle line carries the bytes for the Rust side
                Case c;
                const auto [top, bottom] = GAP_PAIRS[i];
                c.id = "pair-" + std::to_string(top) + "-" + std::to_string(bottom);
                for (uint8_t r = 0; r < 49; r++)
                    c.input.insert(c.input.end(), {top, bottom, r});
                c.length = c.input.size();
                c.inline_input = true;
                return c;
            }
            i -= gap_pairs(p);
            Rng r = Rng::for_case(p.seed ^ 0x676170ULL, i);
            const size_t gaps = 1 + size_t(r.below(p.small ? 20 : 400));
            return generated("seed-" + std::to_string(i), p.seed, i, 3 * gaps, i % 4 == 3 ? Style::Extremes : Style::Random);
        }

        Twin gapfill_twin()
        {
            Twin t;
            t.name = "selftest_gapfill";
            t.about = "the LRPT line repair (pixels from floats), C++ only, built with -ffp-contract=off (rule 1)";
            t.cpp = [](const Case &c) { return gapfill(c, gapfill_strict); };
            t.count = [](const Plan &p) { return gap_pairs(p) + p.cases; };
            t.make = gapfill_case;
            return t;
        }

        Twin stream_twin(const std::string &name, const std::string &about, uint64_t salt, size_t max_length)
        {
            Twin t;
            t.name = name;
            t.about = about;
            t.stream = true;
            t.count = [](const Plan &p) { return p.cases; };
            t.make = [salt, max_length](const Plan &p, uint64_t i)
            { return stream_case(p, i, salt, p.small ? max_length / 20 : max_length); };
            return t;
        }

        Twin splitter(bool bug)
        {
            Twin t = stream_twin("selftest_splitter", "toy record splitter: byte-at-a-time reference against whole records",
                                 0x73706c6974ULL, 4000);
            t.cpp = [](const Case &c) { return split(c, SplitByByte()); };
            t.rust = [bug](const Case &c) { return split(c, SplitByRecord(bug)); };
            t.rust_name = "the second C++";
            return t;
        }
    } // namespace

    void add_selftest_twins(std::vector<Twin> &twins)
    {
        twins.push_back(splitter(false));

        Twin strict = stream_twin("selftest_lowpass", "float filter, C++ only, built with -ffp-contract=off (rule 1)", 0x6c6f77ULL, 2000);
        strict.cpp = [](const Case &c) { return lowpass(c, lowpass_strict); };
        twins.push_back(strict);

        Twin fused = strict;
        fused.name = "selftest_lowpass_contracted";
        fused.about = "the float filter built with the project's flags, within a stated ULP tolerance (rule 3)";
        fused.rust = [](const Case &c) { return lowpass(c, lowpass_contracted); };
        fused.rust_name = "the contracted C++";
        // A first tolerance of 16 ULP looked enough on 2000 cases; the deep run (SD_DIFF_CASES=200000)
        // found 18 ULP, which led to the bound below
        fused.floats = FloatRule::ulps(512, "GCC's default -ffp-contract=fast fuses 0.3f * x + 0.7f * y into one multiply-add on "
                                            "aarch64, one rounding instead of three. While y decays on zero input the gap grows "
                                            "by at most about 1.5 ULP a step, for at most about 250 steps before y is subnormal "
                                            "(where a ULP is a fixed size and the gap shrinks again), so it stays under 512 ULP");
        twins.push_back(fused);

        twins.push_back(gapfill_twin());
    }

    Twin splitter_with_boundary_bug()
    {
        Twin t = splitter(true);
        t.name = "selftest_splitter_bug";
        return t;
    }

    Twin gapfill_with_project_flags()
    {
        Twin t = gapfill_twin();
        t.name = "selftest_gapfill_contracted";
        t.rust = [](const Case &c) { return gapfill(c, gapfill_contracted); };
        t.rust_name = "the contracted C++";
        return t;
    }
} // namespace satdump::diff
