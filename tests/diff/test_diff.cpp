// Fork: the differential-test framework's own tests (diff.h), including the M5.4 proof: a
// one-bit change on the Rust side is found, saved as a reproducer, and the replay shows it again.
// The numbers pinned here are pinned in rust/tests/difftest too, so both sides make the same cases.
#include "diff.h"
#include "doctest.h"
#include "rust_bridge.h"
#include "test_diff.h"
#include <cfloat>
#include <cmath>
#include <fstream>
#include <limits>

using namespace satdump::diff;
namespace fs = std::filesystem;

namespace
{
    std::string hex12(const std::vector<uint8_t> &d)
    {
        static const char digits[] = "0123456789abcdef";
        std::string s;
        for (uint8_t b : d)
            s += digits[b >> 4], s += digits[b & 15];
        return s;
    }

    Record rec(std::initializer_list<Field> f) { return Record(f); }
} // namespace

TEST_CASE("diff: the case generator and digests give the numbers the Rust side pins too")
{
    Rng r(0);
    CHECK(r.next() == 0xe220a8397b1dcdafULL); // SplitMix64's published first output for seed 0
    CHECK(r.next() == 0x6e789e6aa1b965f4ULL);
    Rng c = Rng::for_case(20261112, 7);
    CHECK(c.next() == 0xbacfe2bf7d6ba8b6ULL);
    CHECK(fnv1a(nullptr, 0) == 0xcbf29ce484222325ULL);
    CHECK(fnv1a((const uint8_t *)"foobar", 6) == 0x85944171f73967e8ULL);

    const char *expect[] = {"59d0f5e41c790a880fdaa0df", "000000000000000000000000", "ffffffffffffffffffffffff",
                            "010001000001000001000001", "fffeff00fe01fefe807ffe01", "0000000000000a000f000000"};
    for (int s = 0; s < int(Style::Count); s++)
        CHECK_MESSAGE(hex12(fill(20261112, 3, 12, Style(s))) == expect[s], "style " << s);

    Outcome o;
    o.records = {rec({{"at", Value::integer(5)}, {"payload", Value::data({'a', 'b'})}}),
                 rec({{"y", Value::floats({1.0f, -0.0f})}})};
    CHECK(o.digest() == 0xa1897933d37f8cf4ULL);
}

TEST_CASE("diff: mutations change the bytes and keep the chunks matching")
{
    Case c = generated("m", 1, 2, 40, Style::Zero);
    c.chunks = fixed_chunks(40, 10);
    flip_bit(c, 9);
    CHECK(c.input[1] == 0x40);
    set_byte(c, 0, 0xAB);
    splice(c, 0, 2, 20);
    CHECK(c.input[20] == 0xAB);
    CHECK(c.input[21] == 0x40);
    dup_chunk(c, 2); // bytes 20..29 once more
    CHECK(c.input.size() == 50);
    CHECK(c.input[30] == 0xAB);
    CHECK(c.chunks == std::vector<size_t>{10, 10, 10, 10, 10});
    drop_chunk(c, 0);
    CHECK(c.input.size() == 40);
    CHECK(c.input[10] == 0xAB);
    truncate(c, 25);
    CHECK(c.chunks == std::vector<size_t>{10, 10, 5});
    CHECK(c.mutations.size() == 6);

    // The recorded mutations make the same bytes again from the seed
    std::vector<uint8_t> again = fill(1, 2, 40, Style::Zero);
    for (const Mutation &m : c.mutations)
        apply(again, m);
    CHECK(again == c.input);

    std::vector<uint8_t> small(4);
    CHECK_THROWS(apply(small, {"cut", 2, 3, 0}));
    CHECK_THROWS(apply(small, {"flip", 32, 0, 0}));
    CHECK_THROWS(apply(small, {"copy", UINT64_MAX, 2, 0}));
    CHECK_THROWS(apply(small, {"shuffle", 0, 0, 0}));

    // Random mutations of chunked cases keep the chunks adding up
    Rng r(5);
    for (int i = 0; i < 200; i++)
    {
        Case m = generated("r", 9, uint64_t(i), size_t(r.below(300)), Style::Random);
        m.chunks = random_chunks(r, m.input.size(), 40, true);
        mutate(m, r, 4);
        CHECK_NOTHROW(chunks_of(m));
    }
}

TEST_CASE("diff: ULP distances, with NaN and infinity never close to a number")
{
    CHECK(ulp_distance(1.0f, 1.0f) == 0);
    CHECK(ulp_distance(1.0f, std::nextafter(1.0f, 2.0f)) == 1);
    CHECK(ulp_distance(-1.0f, std::nextafter(-1.0f, -2.0f)) == 1);
    CHECK(ulp_distance(0.0f, -0.0f) == 0);
    CHECK(ulp_distance(-FLT_TRUE_MIN, FLT_TRUE_MIN) == 2);
    CHECK(ulp_distance(0.0f, FLT_TRUE_MIN) == 1);
    CHECK(ulp_distance(std::nanf(""), std::nanf("1")) == 0);
    CHECK(ulp_distance(std::nanf(""), 1.0f) == NOT_CLOSE);
    CHECK(ulp_distance(INFINITY, INFINITY) == 0);
    CHECK(ulp_distance(INFINITY, FLT_MAX) == NOT_CLOSE);
    CHECK(ulp_distance(INFINITY, -INFINITY) == NOT_CLOSE);
    CHECK(ulp_distance(-FLT_MAX, FLT_MAX) == 2 * uint64_t(0x7f7fffff));
    CHECK(ulp_distance(1.0, std::nextafter(1.0, 0.0)) == 1);
    CHECK(ulp_distance(-DBL_MAX, DBL_MAX) == 2 * uint64_t(0x7fefffffffffffffULL));
    CHECK(ulp_distance(std::nan(""), std::nan("")) == 0);
    CHECK(ulp_distance(-INFINITY, -INFINITY) == 0);
}

TEST_CASE("diff: the first difference is named in plain words")
{
    Outcome a, b;
    a.bytes = Value::data({1, 2, 3, 4, 5, 6});
    b.bytes = Value::data({1, 2, 3, 9, 5, 6});
    CHECK(compare(a, a, {}, "C++", "Rust").empty());
    CHECK(compare(a, b, {}, "C++", "Rust") ==
          "bytes: first difference at byte 3 of 6: C++ 0x04, Rust 0x09 (C++ 01 02 03 [04] 05 06, Rust 01 02 03 [09] 05 06)");
    b.bytes = Value::data({1, 2, 3});
    CHECK(compare(a, b, {}, "C++", "Rust") == "bytes: C++ gives 6 bytes, Rust 3 (the same up to there)");
    b = a;
    b.status = 1;
    CHECK(compare(a, b, {}, "C++", "Rust") == "status: C++ 0, Rust 1");

    b = a;
    a.records = {rec({{"apid", Value::integer(64)}, {"payload", Value::data({7, 8})}})};
    b.records = {rec({{"apid", Value::integer(65)}, {"payload", Value::data({7, 8})}})};
    CHECK(compare(a, b, {}, "C++", "Rust") == "record 0 apid: C++ 64, Rust 65");
    b.records = {};
    CHECK(compare(a, b, {}, "C++", "Rust") == "records: C++ gives 1, Rust 0 (the same up to there; the first extra one has fields [apid, payload])");

    // Floats: exact by default; a tolerance allows a few ULP but never NaN against a number
    const float one = 1.0f, next = std::nextafter(1.0f, 2.0f);
    a.records = {rec({{"y", Value::floats({one, 2.0f})}})};
    b.records = {rec({{"y", Value::floats({next, 2.0f})}})};
    CHECK(compare(a, b, {}, "C++", "Rust").find("record 0 y: value 0 of 2: C++ 1 (0x3f800000), Rust 1.00000012 (0x3f800001), 1 ULP apart, 0 allowed") == 0);
    CHECK(compare(a, b, FloatRule::ulps(1, "test"), "C++", "Rust").empty());
    b.records = {rec({{"y", Value::floats({std::nanf(""), 2.0f})}})};
    CHECK(compare(a, b, FloatRule::ulps(1000, "test"), "C++", "Rust").find("not comparable") != std::string::npos);
    a.records = {rec({{"y", Value::floats({0.0f})}})};
    b.records = {rec({{"y", Value::floats({-0.0f})}})};
    CHECK(compare(a, b, {}, "C++", "Rust").find("equal as numbers but not bit for bit") != std::string::npos);
    CHECK(compare(a, b, FloatRule::ulps(1, "test"), "C++", "Rust").empty());
}

TEST_CASE("diff: an oracle line gives back its case and outcome, in full or as digests")
{
    const Twin &twin = *find_twin("selftest_splitter");
    Plan plan;
    plan.cases = 40;
    for (uint64_t i = 0; i < plan.cases; i++)
    {
        const Case c = twin.make(plan, i);
        const Outcome out = twin.cpp(c);
        for (size_t inline_max : {size_t(0), size_t(256), SIZE_MAX})
        {
            const OracleLine o = parse_oracle_line(oracle_line(twin, c, out, inline_max), ".");
            CHECK(o.twin == "selftest_splitter");
            CHECK(o.c.input == c.input);
            CHECK(o.c.chunks == c.chunks);
            CHECK(compare(o.out, out, {}, "the oracle", "C++").empty());
        }
    }
    // A digest still tells a difference: a case with a payload, kept as digests only
    uint64_t i = 0;
    while (twin.cpp(twin.make(plan, i)).records.empty() || twin.cpp(twin.make(plan, i)).records[0][2].value.bytes.empty())
        REQUIRE(++i < plan.cases);
    const Case c = twin.make(plan, i);
    Outcome out = twin.cpp(c);
    const OracleLine o = parse_oracle_line(oracle_line(twin, c, out, 0), ".");
    REQUIRE(o.out.records[0][2].value.summary);
    out.status++;
    CHECK_FALSE(compare(o.out, out, {}, "the oracle", "C++").empty());
    out.status--;
    out.records[0][2].value.bytes[0] ^= 1;
    CHECK(compare(o.out, out, {}, "the oracle", "C++").find("only a digest was kept") != std::string::npos);
    out.records[0][2].value.bytes[0] ^= 1;
    out.records.push_back(rec({{"at", Value::integer(0)}}));
    CHECK(compare(o.out, out, {}, "the oracle", "C++").find("records: the oracle gives") == 0);
}

TEST_CASE("diff: every twin agrees with its counterpart on a short plan")
{
    TempOptions t;
    Plan plan;
    plan.cases = 50;
    for (const Twin &twin : all_twins())
        if (twin.rust)
        {
            const Result r = run(twin, plan, t.opt);
            CHECK_MESSAGE(r.differences == 0, twin.name << ": " << r.first);
            CHECK(r.cases == twin.count(plan));
        }
}

TEST_CASE("diff: a one-bit change on the Rust side is found, saved and replayed (M5.4)")
{
    REQUIRE(satdump::rust::enabled());
    TempOptions t;
    const Twin flipped = derand_with_flipped_bit(1020, 100);
    Plan plan;
    plan.cases = 20;
    const Result r = run(flipped, plan, t.opt);
    // Every case of 1020 bytes (len-1020 in the sweep, edge length 1020 in six fill styles at
    // two offsets, and any seeded one) and no other
    uint64_t length_1020 = 0;
    for (uint64_t i = 0; i < flipped.count(plan); i++)
        length_1020 += flipped.make(plan, i).input.size() == 1020;
    CHECK(length_1020 >= 13);
    CHECK(r.differences == length_1020);
    REQUIRE(r.saved.size() == 3);
    CHECK(r.first.find("bytes: first difference at byte 104 of 1032") == 0); // the sweep's offset is 1020 % 8 = 4
    for (const char *file : {"input.bin", "case.json", "expected.json", "actual.json"})
        CHECK_MESSAGE(fs::exists(fs::path(r.saved[0]) / file), file);
    CHECK(fs::file_size(fs::path(r.saved[0]) / "input.bin") == 1020);

    // The replay shows it again with the changed side, and not with the real one
    const Result again = replay(r.saved[0], &flipped);
    CHECK(again.differences == 1);
    CHECK(again.first == r.first);
    const Twin *real = find_twin("derand_ccsds");
    CHECK(replay(r.saved[0], real).differences == 0);

    // Against an oracle written by the C++ the changed side fails too
    const fs::path oracle = fs::path(t.opt.failures) / "derand.jsonl";
    {
        std::ofstream f(oracle);
        Plan small;
        small.small = true;
        small.cases = 4;
        for (uint64_t i = 0; i < real->count(small); i++)
        {
            const Case c = real->make(small, i);
            f << oracle_line(*real, c, real->cpp(c), 256) << "\n";
        }
    }
    Options o = t.opt;
    o.failures += "/from-oracle";
    CHECK(check_oracle(oracle.string(), o).differences == 0);
    const Result checked = check_oracle(oracle.string(), o, &flipped);
    CHECK(checked.differences == 1); // edge length 1020
    REQUIRE(checked.saved.size() == 1);
    CHECK(replay(checked.saved[0], &flipped).differences == 1);
    CHECK(replay(checked.saved[0], real).differences == 0);
}

TEST_CASE("diff: a decoder bug that shows only across a chunk boundary is found, and the chunks reproduce it")
{
    TempOptions t;
    const Twin buggy = splitter_with_boundary_bug();
    Plan plan;
    plan.cases = 200;
    const Result r = run(buggy, plan, t.opt);
    CHECK(r.differences > 0);
    REQUIRE(!r.saved.empty());
    CHECK(replay(r.saved[0], &buggy).differences == 1);

    // Each failing case passes when the same bytes come in one piece
    Options quiet = t.opt;
    quiet.max_saved = 0;
    quiet.max_printed = 0;
    for (uint64_t i = 0; i < plan.cases; i++)
    {
        Case c = buggy.make(plan, i);
        if (run_cases(buggy, {c}, quiet).differences == 0)
            continue;
        CHECK(c.chunks.size() > 1);
        c.chunks.clear();
        CHECK(run_cases(buggy, {c}, quiet).differences == 0);
    }
}

TEST_CASE("diff: corpus files become one case per CADU, or one chunked case for a stream twin")
{
    TempOptions t;
    fs::create_directories(t.opt.failures);
    const fs::path file = fs::path(t.opt.failures) / "two.cadu";
    {
        std::ofstream f(file, std::ios::binary);
        f << std::string(2 * 1024 + 100, '\x5a');
    }
    const std::vector<Case> frames = corpus_cases(*find_twin("derand_ccsds"), file.string());
    REQUIRE(frames.size() == 3); // two whole frames and the 100 bytes at the end
    CHECK(frames[1].input.size() == 1020);
    CHECK(frames[1].at == 1024 + 4);
    CHECK(frames[2].input.size() == 96);
    CHECK(run_cases(*find_twin("derand_ccsds"), frames, t.opt).differences == 0);
    const std::vector<Case> stream = corpus_cases(*find_twin("selftest_splitter"), file.string());
    REQUIRE(stream.size() == 1);
    CHECK(stream[0].input.size() == 2 * 1024 + 100);

    // A folder, as a fuzz corpus (tests/fuzz/corpus/<harness>): every file, in name order;
    // a file no longer than the part skipped at the start of a frame gives no case
    const fs::path folder = fs::path(t.opt.failures) / "corpus";
    fs::create_directories(folder);
    std::ofstream(folder / "b", std::ios::binary) << std::string(10, '\x01');
    std::ofstream(folder / "a", std::ios::binary) << std::string(3, '\x02');
    const std::vector<Case> fuzz = corpus_cases(*find_twin("derand_ccsds"), folder.string());
    REQUIRE(fuzz.size() == 1);
    CHECK(fuzz[0].id == "b@4");
    CHECK(fuzz[0].input == std::vector<uint8_t>(6, 0x01));
}
