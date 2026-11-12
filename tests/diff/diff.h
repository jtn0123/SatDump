#pragma once

// Fork: differential tests for SatDump's Rust parts (FORK.md, "Rust parts"). Built only in
// Rust builds with tests (tests/diff/diff.cmake).
//
// A twin is a C++ reference function and its Rust counterpart with the same buffer contract,
// for example derand_ccsds_cpp and satdump::rust::derand_ccsds. Both get the same cases and
// must give the same outcome.
//
// - A case is input bytes and how they are fed: in one call (pure buffer functions: the
//   derandomiser today, Reed-Solomon and deframers later), or as a list of chunk lengths for
//   a stateful decoder fed a stream (the AOS demuxer, the LRPT segment decoder), so chunk
//   boundaries are part of the case. Named integer parameters carry the rest (a buffer
//   offset, a length to pass instead of the real one, a mode).
// - An outcome is a status code, output bytes and a list of records (packets, segments).
//   A record is an ordered list of named values: integers, bytes, 32- or 64-bit floats.
// - Cases come from edge lists, from a seeded generator (SD_DIFF_SEED; SD_DIFF_CASES seeded
//   cases per twin, 2000 by default, 200000 for a deep run), from files (the golden
//   recordings, fuzz corpora: `satdump-diff run --corpus <file or folder>`) and from mutating
//   them (bit flips, changed bytes, truncation, splices, dropped or repeated chunks). A case
//   is described by data alone (seed, index, length, fill style, corpus file, mutations,
//   chunks, parameters), so the Rust tests rebuild it from a JSONL oracle line without the
//   C++. A case from a structured generator the Rust tests do not have (CADUs packed with
//   packets, for example) sets inline_input, and its oracle line carries the bytes.
// - The C++ side of a comparison runs with Rust paused (satdump::rust::CppOnly), so it stays
//   the C++ code even where SatDump calls Rust. An exception on either side is a difference
//   like any other.
//
// Oracles: `satdump-diff oracle` writes what the C++ twin gives, one JSON line per case.
// Small ones, with every output in full, are committed in tests/diff/oracles; a build writes
// those of the full plan to <build>/diff-oracles (long outputs as digests). `cargo test`
// replays them in Rust alone (rust/tests/difftest/main.rs); `satdump-diff check` checks
// that the C++ still gives them and Rust too.
//
// Floating point. Rust must give what SatDump's C++ gives as it ships, so the C++ side of a
// twin is always the production code compiled with the build's own flags, never a copy
// compiled another way. Integers, bytes and floats must match bit for bit. When optimising,
// GCC (gnu++17) fuses a * b + c into one multiply-add with a single rounding on aarch64, and
// Rust never does, so the port of float code chooses one of these in its FORK.md row:
//   1. The default: the C++ sources it ports are compiled with -ffp-contract=off in Rust-on
//      builds only (set_source_files_properties(<files> PROPERTIES COMPILE_OPTIONS
//      -ffp-contract=off) in a block that runs only when SATDUMP_WITH_RUST is ON, such as
//      cmake/fork/rust.cmake), so Rust-off builds keep their flags; golden and golden_no_rust
//      are checked, and the Rust uses plain arithmetic. Both then give the same bits on every
//      machine and build type. Integer-only code (the AOS splitter, the LRPT segment
//      decoder) needs no flag.
//   2. Where the C++ itself calls std::fma, the Rust calls mul_add.
//   3. Only for float outputs no flag makes exact (a libm function, for example): a ULP
//      tolerance (FloatRule::ulps) with its reason written next to it. A tolerance never
//      covers integers, so a float turned into an integer (an image pixel) must match exactly
//      through 1 or 2. A NaN or an infinity never matches a number, whatever the tolerance.
// The self-test twins (twins_selftest.cpp) are built as rule 1 says. test_diff_rules.cpp shows
// that the same code built with the project's flags gives other floats, and other pixels in
// selftest_gapfill (an integer from a float, as the Meteor LRPT line repair), which no
// tolerance hides.
//
// The first differences (SD_DIFF_MAX_SAVED, 3 by default) are saved as reproducers
// (input.bin, case.json, expected.json, actual.json) in <build>/diff-failures/<twin>-<case>/
// (SD_DIFF_FAILURES), and one line names the first differing byte or record and the replay
// command: `<build>/satdump-diff replay <folder>`, or on the Rust side alone
// `SD_DIFF_REPLAY=<folder> cargo test --release --manifest-path rust/Cargo.toml --test difftest
// replay_a_reproducer`. case.json has both commands in full.

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace satdump::diff
{
    // ---- Seeded cases (cases.cpp). The Rust side has the same code (rust/tests/difftest/rng.rs).

    // SplitMix64: small, and easy to write the same way in both languages
    class Rng
    {
    public:
        explicit Rng(uint64_t seed) : state(seed) {}
        // The generator for case `index` of a run with `seed`
        static Rng for_case(uint64_t seed, uint64_t index);
        uint64_t next();
        uint64_t below(uint64_t n); // 0 .. n-1, and 0 when n is 0

    private:
        uint64_t state;
    };

    constexpr uint64_t FNV_OFFSET = 0xCBF29CE484222325ULL;
    uint64_t fnv1a(const uint8_t *data, size_t size, uint64_t h = FNV_OFFSET);

    // How the bytes of a generated case are filled
    enum class Style
    {
        Random,   // every byte from the generator
        Zero,     // all 0x00
        Ones,     // all 0xFF
        Bits,     // 0 or 1
        Extremes, // 0x00, 0x01, 0x7F, 0x80, 0xFE or 0xFF
        Sparse,   // mostly 0x00, a few small values
        Count
    };
    std::vector<uint8_t> fill(uint64_t seed, uint64_t index, size_t length, Style style);

    // One change to the bytes of a case, kept so the Rust side can make the same input:
    //   flip a        flip bit a (bit 0 is the top bit of byte 0)
    //   set a b       byte a becomes b
    //   truncate a    keep the first a bytes
    //   copy a b c    copy b bytes from position a over position c
    //   cut a b       remove b bytes at a
    //   repeat a b    insert a second copy of the b bytes at a right after them
    struct Mutation
    {
        std::string op;
        uint64_t a = 0, b = 0, c = 0;
    };
    void apply(std::vector<uint8_t> &data, const Mutation &m); // throws on a bad mutation

    struct Case
    {
        std::string id;
        uint64_t seed = 0, index = 0; // fill(seed, index, length, style) made the bytes...
        uint64_t length = 0;
        Style style = Style::Random;
        std::string corpus; // ...or they are `length` bytes at `at` in this file
        uint64_t at = 0;
        std::vector<Mutation> mutations;              // then changed by these
        std::vector<size_t> chunks;                   // stream twins: fed in these pieces
        std::map<std::string, int64_t> params;        // twin-specific settings
        std::vector<uint8_t> input;                   // the bytes themselves
        bool inline_input = false;                    // oracle lines carry the bytes (no generator in Rust)

        int64_t param(const std::string &name, int64_t fallback) const;
    };

    Case generated(const std::string &id, uint64_t seed, uint64_t index, size_t length, Style style);

    // Mutation helpers; they keep `chunks` matching the bytes
    void flip_bit(Case &c, uint64_t bit);
    void set_byte(Case &c, uint64_t pos, uint8_t value);
    void truncate(Case &c, uint64_t length);
    void splice(Case &c, uint64_t from, uint64_t length, uint64_t to);
    void drop_chunk(Case &c, size_t k);
    void dup_chunk(Case &c, size_t k);
    void mutate(Case &c, Rng &rng, int count); // `count` random ones of the above

    // The chunks a case is fed in: its own, or the whole input as one (throws if they do not add up)
    std::vector<size_t> chunks_of(const Case &c);
    // Ways to cut `total` bytes into chunks
    std::vector<size_t> fixed_chunks(size_t total, size_t size);
    std::vector<size_t> random_chunks(Rng &rng, size_t total, size_t max_chunk, bool empty_chunks);
    // A length up to `max`: short, anywhere, near a multiple of one of `near`, or near `max`
    size_t pick_length(Rng &rng, size_t max, const std::vector<size_t> &near);

    // ---- Outcomes (outcome.cpp)

    struct Value
    {
        enum class Kind
        {
            Int,
            Bytes,
            F32,
            F64
        };
        Kind kind = Kind::Int;
        int64_t i = 0;
        std::vector<uint8_t> bytes; // Bytes
        std::vector<uint32_t> f32;  // F32, as bit patterns
        std::vector<uint64_t> f64;  // F64, as bit patterns
        // Read from an oracle line that keeps only the length and digest of the data
        bool summary = false;
        uint64_t length = 0, digest = 0;

        uint64_t len() const;
        uint64_t fnv() const;
        static Value integer(int64_t v);
        static Value data(std::vector<uint8_t> v);
        static Value floats(const std::vector<float> &v);
        static Value doubles(const std::vector<double> &v);
    };

    struct Field
    {
        std::string name;
        Value value;
    };
    using Record = std::vector<Field>;

    struct Outcome
    {
        int64_t status = 0;
        Value bytes = Value::data({});
        std::vector<Record> records;
        // Read from an oracle line that keeps only how many records there were and a digest
        bool records_summary = false;
        uint64_t records_count = 0, records_digest = 0;

        uint64_t count() const;
        uint64_t digest() const; // of the records, from the values' digests
    };

    // How floats are compared. Exact (max_ulps 0, the default): the bits must be equal.
    // With a tolerance, finite values may be up to max_ulps apart (+0 and -0 are 0 apart), an
    // infinity matches only the same infinity, and a NaN matches any NaN. Integers and bytes
    // are always exact.
    struct FloatRule
    {
        uint64_t max_ulps = 0;
        std::string reason; // why a twin needs a tolerance
        static FloatRule ulps(uint64_t n, const std::string &why);
    };
    constexpr uint64_t NOT_CLOSE = UINT64_MAX; // ulp_distance of a NaN or infinity and anything else
    uint64_t ulp_distance(float a, float b);
    uint64_t ulp_distance(double a, double b);

    // "" when `got` gives the same as `want`, else the first difference in plain words
    std::string compare(const Outcome &want, const Outcome &got, const FloatRule &rule,
                        const std::string &want_name, const std::string &got_name);

    // ---- Twins (run.cpp, twins_*.cpp)

    struct Plan
    {
        uint64_t seed = 20261112;
        uint64_t cases = 2000; // seeded cases per twin
        bool small = false;    // oracle-sized: edge cases and short inputs only, for committed files
    };

    using Runner = std::function<Outcome(const Case &)>;

    struct Twin
    {
        std::string name, about;
        Runner cpp;  // the C++ reference
        Runner rust; // its counterpart, through rust/bridge/rust_bridge.h
        std::string rust_name = "Rust";
        FloatRule floats;
        std::function<uint64_t(const Plan &)> count;          // how many cases a plan has
        std::function<Case(const Plan &, uint64_t)> make;     // case i of a plan
        // Fork part "aos": the group a case counts under (a layout, say), or none; `satdump-diff
        // run` then prints each group's cases and chunks, so a deep run shows where they went
        std::function<std::string(const Case &)> group;
        bool stream = false;      // fed in chunks (a corpus file is one case cut into frames)
        size_t corpus_frame = 0;  // corpus files are cut into frames of this size (0: whole)
        size_t corpus_skip = 0;   // buffer twins work on each frame from this byte on
        // LRPT (M6.3): when set, reads each corpus file (its name, its bytes) into cases itself,
        // in place of the frames above; lrpt_segment takes the MSU-MR packets out of CADUs
        std::function<std::vector<Case>(const std::string &, const std::vector<uint8_t> &)> corpus_reader;
    };

    void add_derand_twins(std::vector<Twin> &twins);
    void add_selftest_twins(std::vector<Twin> &twins);
    // Fork part "aos": the CCSDS AOS demuxer (twins_aos.cpp)
    void add_aos_twins(std::vector<Twin> &twins);
    // LRPT (M6.3): the Meteor LRPT segment decoder (twins_lrpt.cpp)
    void add_lrpt_twins(std::vector<Twin> &twins);

    // For the framework's own tests (test_diff*.cpp): twins with a deliberate fault
    Twin derand_with_flipped_bit(size_t length, size_t byte); // the real Rust, one bit flipped for this input length
    Twin splitter_with_boundary_bug();                        // forgets a record header cut by a chunk boundary
    Twin gapfill_with_project_flags();                        // selftest_gapfill built without rule 1 (diff.h)
    // The float self-test code, each compiled twice (floats.cpp): with -ffp-contract=off as
    // rule 1 says (strict), and with the project's own flags (contracted)
    float lowpass_strict(float y, const uint8_t *in, size_t n, float *out);
    float lowpass_contracted(float y, const uint8_t *in, size_t n, float *out);
    void gapfill_strict(uint8_t top, uint8_t bottom, size_t range, uint8_t *out);
    void gapfill_contracted(uint8_t top, uint8_t bottom, size_t range, uint8_t *out);
    const std::vector<Twin> &all_twins();
    const Twin *find_twin(const std::string &name);

    struct Options
    {
        std::string failures;   // where reproducers go
        int max_saved = 3;      // reproducers per run
        int max_printed = 10;   // difference lines per run
        uint64_t min_cases = 1; // oracle lines each checked file must have
    };
    // The commands that replay a reproducer folder: with satdump-diff, and with cargo alone
    std::string replay_command(const std::string &folder);
    std::string rust_replay_command(const std::string &folder);
    // What a twin's side throws comes back as this status, with the message as a record
    constexpr int64_t STATUS_THREW = INT64_MIN;
    Options options_from_env();
    Plan plan_from_env();

    struct Result
    {
        uint64_t cases = 0, differences = 0;
        std::vector<std::string> saved; // reproducer folders
        std::string first;              // the first difference
        // Fork part "aos": the cases and chunks fed (empty ones not counted) of each Twin::group
        struct Group
        {
            uint64_t cases = 0, chunks = 0;
        };
        std::map<std::string, Group> groups;
    };

    // What the C++ side gives, run with Rust paused (for oracles); throws, naming the case, if it throws
    Outcome cpp_outcome(const Twin &twin, const Case &c);
    // C++ against the counterpart on these cases
    Result run(const Twin &twin, const Plan &plan, const Options &opt);
    Result run_cases(const Twin &twin, const std::vector<Case> &cases, const Options &opt);
    // Cases from a file, or every file in a folder (golden recordings, fuzz corpora). The file
    // paths are kept absolute, or relative to `relative_to` (the folder of an oracle file).
    std::vector<Case> corpus_cases(const Twin &twin, const std::string &path, const std::string &relative_to = "");

    // ---- Oracles and reproducers (io.cpp)

    // One JSONL line: the case, the float rule and what the C++ twin gave. Outputs longer than
    // `inline_max` bytes are kept as length and digest only.
    std::string oracle_line(const Twin &twin, const Case &c, const Outcome &cpp, size_t inline_max);
    struct OracleLine
    {
        std::string twin;
        Case c; // with its bytes made again (corpus paths are relative to `folder`)
        FloatRule floats;
        Outcome out;
    };
    OracleLine parse_oracle_line(const std::string &line, const std::string &folder);
    struct Saved
    {
        std::string twin, source; // source: "live", or the oracle line it came from
        Case c;
        Outcome expected;
    };
    Saved load_reproducer(const std::string &folder);
    // Every line of an oracle file (or of each .jsonl file in a folder) is run again through
    // the C++ twin (is the oracle still current?) and its counterpart; `twin`, when given,
    // stands in for the one each line names. A file with fewer than opt.min_cases lines, or a
    // line that cannot be read, is an error (thrown, with the file and line).
    Result check_oracle(const std::string &path, const Options &opt, const Twin *twin = nullptr);
    std::string save_reproducer(const Options &opt, const Twin &twin, const Case &c,
                                const Outcome &want, const Outcome &got, const std::string &want_name,
                                const std::string &got_name, const std::string &difference,
                                const std::string &source);
    // Runs a saved case again through both sides of its twin (or of `twin` when given); a case
    // saved from an oracle is also checked against the outcome saved with it
    Result replay(const std::string &folder, const Twin *twin);
} // namespace satdump::diff
