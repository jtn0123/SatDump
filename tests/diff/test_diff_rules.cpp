// Fork: the differential-test framework's own tests (diff.h), part two: the float rule, the
// two sides of a comparison (Rust paused for the C++ side, exceptions), oracle files
// (structured cases, corpus paths, the minimum number of lines) and the settings.
#include "diff.h"
#include "doctest.h"
#include "rust_bridge.h"
#include "test_diff.h"
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <stdexcept>

using namespace satdump::diff;
namespace fs = std::filesystem;

TEST_CASE("diff: the float rule - the project's flags change float results; a stated tolerance covers floats")
{
    TempOptions t;
    Twin exact = *find_twin("selftest_lowpass_contracted");
    const uint64_t tolerance = exact.floats.max_ulps;
    exact.floats = FloatRule();
    Plan plan;
    plan.cases = 100;
    const Result r = run(exact, plan, t.opt);
#if defined(__aarch64__) && defined(__OPTIMIZE__)
    // GCC fuses 0.3f * x + 0.7f * y into one multiply-add on aarch64 when optimising, unless
    // told not to (this file is built with the same flags as the contracted copy)
    CHECK(r.differences > 0);
    CHECK(r.first.find("ULP apart, 0 allowed") != std::string::npos);
#endif
    MESSAGE("exact rule: " << r.differences << " of " << r.cases << " cases differ; first: " << r.first);
    exact.floats = FloatRule::ulps(tolerance, "test");
    CHECK(run(exact, plan, t.opt).differences == 0);
}

TEST_CASE("diff: the float rule - a pixel made from a float differs with the project's flags, and no tolerance hides it")
{
    TempOptions t;
    Twin flags = gapfill_with_project_flags();
    Plan plan;
    plan.cases = 100;
    const Result r = run(flags, plan, t.opt);
    MESSAGE("selftest_gapfill, strict against the project's flags: " << r.differences << " of " << r.cases
                                                                      << " cases differ; first: " << r.first);
#if defined(__aarch64__) && defined(__OPTIMIZE__)
    // The Meteor LRPT line repair is built so today (lrpt_msumr_reader.cpp): a port of it has to
    // follow rule 1 (diff.h), as these integers cannot be compared within a tolerance
    CHECK(r.differences > 0);
    CHECK(r.first.find(" pixels: first difference at byte ") != std::string::npos);
#endif
    // A float tolerance, however large, does not apply to integers and bytes
    flags.floats = FloatRule::ulps(uint64_t(1) << 40, "test");
    CHECK(run(flags, plan, t.opt).differences == r.differences);
    // The same code built as rule 1 says is the reference and agrees with itself
    Twin same = flags;
    same.rust = same.cpp;
    CHECK(run(same, plan, t.opt).differences == 0);
}

TEST_CASE("diff: the C++ side runs with Rust paused, and an exception on either side is a difference")
{
    TempOptions t;
    REQUIRE(satdump::rust::enabled());
    // The C++ side sees Rust off and the counterpart sees it on: so a C++ reference that
    // SatDump routes through Rust (as derand_ccsds) still runs its C++ code in a comparison
    Twin probe = *find_twin("derand_ccsds");
    probe.name = "probe";
    probe.cpp = [](const Case &)
    {
        Outcome o;
        o.status = satdump::rust::enabled() ? 1 : 0;
        return o;
    };
    probe.rust = [](const Case &)
    {
        Outcome o;
        o.status = satdump::rust::enabled() ? 0 : 1;
        return o;
    };
    Plan plan;
    plan.cases = 3;
    CHECK(run(probe, plan, t.opt).differences == 0);
    CHECK(satdump::rust::enabled());

    // A throwing side: counted, named with its message, saved and replayed
    Twin throws = *find_twin("derand_ccsds");
    throws.name = "throws";
    throws.rust = [](const Case &c) -> Outcome
    {
        if (c.input.size() == 7)
            throw std::runtime_error("seven bytes");
        return Outcome();
    };
    throws.cpp = [](const Case &) { return Outcome(); };
    const Result r = run(throws, plan, t.opt);
    CHECK(r.differences >= 1);
    CHECK(r.first == "Rust threw an exception: seven bytes");
    REQUIRE(!r.saved.empty());
    CHECK(replay(r.saved[0], &throws).differences == 1);
    std::ifstream f(fs::path(r.saved[0]) / "case.json");
    const std::string about((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    // The replay commands name the tool and the crate in full
    CHECK(about.find(replay_command(r.saved[0])) != std::string::npos);
    CHECK(about.find(rust_replay_command(r.saved[0])) != std::string::npos);
    CHECK(fs::path(replay_command("x").substr(0, replay_command("x").find(' '))).is_absolute());
    CHECK(rust_replay_command("x").find("--manifest-path /") != std::string::npos);

    Twin cpp_throws = throws;
    cpp_throws.cpp = throws.rust;
    cpp_throws.rust = [](const Case &) { return Outcome(); };
    CHECK(run(cpp_throws, plan, t.opt).first == "C++ threw an exception: seven bytes");
    CHECK_THROWS_WITH(cpp_outcome(cpp_throws, cpp_throws.make(plan, 7)),
                      doctest::Contains("case len-7: the C++ side threw an exception: seven bytes"));
}

TEST_CASE("diff: structured cases carry their bytes, corpus paths are relative to the oracle, and float records are all kept")
{
    TempOptions t;
    fs::create_directories(t.opt.failures);
    // A case from a generator the Rust side does not have: its bytes are in the line
    const Twin &gapfill = *find_twin("selftest_gapfill");
    Plan plan;
    const Case pair = gapfill.make(plan, 0);
    REQUIRE(pair.inline_input);
    const std::string line = oracle_line(gapfill, pair, cpp_outcome(gapfill, pair), 256);
    CHECK(line.find("\"input\":\"070100") != std::string::npos);
    const OracleLine back = parse_oracle_line(line, ".");
    CHECK(back.c.input == pair.input);
    CHECK(back.c.inline_input);
    std::string changed = line;
    changed.replace(changed.find("070100"), 6, "070200");
    CHECK_THROWS_WITH(parse_oracle_line(changed, "."), doctest::Contains("no longer makes the same bytes"));

    // Corpus cases written relative to the oracle file's folder read back from there
    const fs::path folder = fs::path(t.opt.failures) / "oracles";
    fs::create_directories(folder / "corpus");
    std::ofstream(folder / "corpus" / "one.cadu", std::ios::binary) << std::string(1024, '\x33');
    const Twin &derand = *find_twin("derand_ccsds");
    const std::vector<Case> cases = corpus_cases(derand, (folder / "corpus").string(), folder.string());
    REQUIRE(cases.size() == 1);
    CHECK(cases[0].corpus == "corpus/one.cadu");
    {
        std::ofstream f(folder / "derand_ccsds.jsonl");
        f << oracle_line(derand, cases[0], cpp_outcome(derand, cases[0]), 256) << "\n";
    }
    CHECK(check_oracle(folder.string(), t.opt).cases == 1);

    // Fewer lines than asked for, or a damaged line, is an error naming the file and line
    Options more = t.opt;
    more.min_cases = 2;
    CHECK_THROWS_WITH(check_oracle(folder.string(), more), doctest::Contains("has 1 cases, fewer than 2"));
    std::ofstream(folder / "derand_ccsds.jsonl") << "\n{\"twin\": \"derand_ccsds\"\n";
    CHECK_THROWS_WITH(check_oracle(folder.string(), t.opt), doctest::Contains("derand_ccsds.jsonl:2: "));

    // A twin with a float tolerance keeps every record, however many, so replays can apply it
    const Twin &fused = *find_twin("selftest_lowpass_contracted");
    Case many = generated("many", 1, 1, 300, Style::Random);
    many.chunks = fixed_chunks(300, 1);
    const Outcome out = fused.cpp(many);
    REQUIRE(out.records.size() == 300);
    CHECK_FALSE(parse_oracle_line(oracle_line(fused, many, out, 256), ".").out.records_summary);
    CHECK(parse_oracle_line(oracle_line(*find_twin("selftest_lowpass"), many, out, 256), ".").out.records_summary);
}

TEST_CASE("diff: SD_DIFF_SEED and SD_DIFF_CASES are decimal")
{
    setenv("SD_DIFF_SEED", "010", 1);
    setenv("SD_DIFF_CASES", "12", 1);
    CHECK(plan_from_env().seed == 10);
    CHECK(plan_from_env().cases == 12);
    for (const char *bad : {"0x10", "-1", "+5", " 7", "08x"})
    {
        setenv("SD_DIFF_SEED", bad, 1);
        CHECK_THROWS(plan_from_env());
    }
    unsetenv("SD_DIFF_SEED");
    unsetenv("SD_DIFF_CASES");
    CHECK(plan_from_env().seed == 20261112);
}
