// Fork: running twins (diff.h): live C++ against Rust, oracle checks, replays, the list of
// twins and the settings from the environment.
#include "diff.h"
#include "rust_bridge.h"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

#ifndef SATDUMP_DIFF_FAILURES
#define SATDUMP_DIFF_FAILURES "diff-failures"
#endif
#ifndef SATDUMP_DIFF_TOOL
#define SATDUMP_DIFF_TOOL "satdump-diff"
#endif
#ifndef SATDUMP_DIFF_CARGO
#define SATDUMP_DIFF_CARGO "cargo"
#endif
#ifndef SATDUMP_DIFF_CARGO_TOML
#define SATDUMP_DIFF_CARGO_TOML "rust/Cargo.toml"
#endif

namespace fs = std::filesystem;

namespace satdump::diff
{
    namespace
    {
        uint64_t env_number(const char *name, uint64_t fallback)
        {
            const char *v = std::getenv(name);
            if (v == nullptr || v[0] == '\0')
                return fallback;
            // Decimal digits only (no sign, no 0x, and a leading 0 is not octal)
            const std::string text = v;
            errno = 0;
            const unsigned long long n = std::strtoull(v, nullptr, 10);
            if (!std::all_of(text.begin(), text.end(), [](char ch) { return std::isdigit((unsigned char)ch) != 0; }) || errno != 0)
                throw std::invalid_argument(std::string(name) + "=" + v + " is not a decimal number");
            return n;
        }

        // One side of a comparison. The C++ side runs with Rust paused, so it stays the C++ code
        // even where SatDump calls Rust. An exception comes back as STATUS_THREW and its message.
        Outcome run_side(const Runner &side, const Case &c, bool cpp)
        {
            try
            {
                if (!cpp)
                    return side(c);
                const satdump::rust::CppOnly paused;
                return side(c);
            }
            catch (const std::exception &e)
            {
                const std::string what = e.what();
                Outcome o;
                o.status = STATUS_THREW;
                o.records = {{{"exception", Value::data(std::vector<uint8_t>(what.begin(), what.end()))}}};
                return o;
            }
        }

        std::string threw(const Outcome &o, const std::string &name)
        {
            if (o.status != STATUS_THREW || o.records.size() != 1 || o.records[0].empty())
                return "";
            const std::vector<uint8_t> &what = o.records[0][0].value.bytes;
            return name + " threw an exception: " + std::string(what.begin(), what.end());
        }

        // The first difference, an exception on either side first
        std::string differ(const Outcome &want, const Outcome &got, const FloatRule &rule, const std::string &wn,
                           const std::string &gn)
        {
            const std::string w = threw(want, wn), g = threw(got, gn);
            return !w.empty() ? w : !g.empty() ? g : compare(want, got, rule, wn, gn);
        }

        // Counts a difference, saves the first ones and prints one line for each of the first few
        void note(Result &r, const Options &opt, const Twin &twin, const Case &c, const Outcome &want,
                  const Outcome &got, const std::string &wn, const std::string &gn, const std::string &d,
                  const std::string &source)
        {
            r.differences++;
            if (r.first.empty())
                r.first = d;
            std::string where;
            if (int(r.saved.size()) < opt.max_saved)
            {
                r.saved.push_back(save_reproducer(opt, twin, c, want, got, wn, gn, d, source));
                where = "\n      reproducer: " + r.saved.back() + "\n      replay it: " + replay_command(r.saved.back());
            }
            if (r.differences <= uint64_t(opt.max_printed))
                std::cout << "diff: " << twin.name << " case " << c.id << " (" << source << "): " << d << where << std::endl;
        }

        void check(Result &r, const Twin &twin, const Case &c, const Options &opt)
        {
            r.cases++;
            // Fork part "aos": where the cases went (Twin::group)
            if (twin.group)
            {
                Result::Group &g = r.groups[twin.group(c)];
                g.cases++;
                g.chunks += uint64_t(std::count_if(c.chunks.begin(), c.chunks.end(), [](size_t n) { return n > 0; }));
            }
            const Outcome cpp = run_side(twin.cpp, c, true);
            const Outcome rs = run_side(twin.rust, c, false);
            const std::string d = differ(cpp, rs, twin.floats, "C++", twin.rust_name);
            if (!d.empty())
                note(r, opt, twin, c, cpp, rs, "C++", twin.rust_name, d, "live");
        }

        void need_counterpart(const Twin &twin)
        {
            if (!twin.rust)
                throw std::runtime_error(twin.name + " has no Rust counterpart yet (only oracles can be written)");
        }
    } // namespace

    Outcome cpp_outcome(const Twin &twin, const Case &c)
    {
        const Outcome o = run_side(twin.cpp, c, true);
        const std::string t = threw(o, "the C++ side");
        if (!t.empty())
            throw std::runtime_error(twin.name + " case " + c.id + ": " + t);
        return o;
    }

    std::string replay_command(const std::string &folder)
    {
        return std::string(SATDUMP_DIFF_TOOL) + " replay " + folder;
    }

    std::string rust_replay_command(const std::string &folder)
    {
        std::string target;
#ifdef SATDUMP_DIFF_CARGO_TARGET
        target = std::string("CARGO_TARGET_DIR=") + SATDUMP_DIFF_CARGO_TARGET + " ";
#endif
        return "SD_DIFF_REPLAY=" + folder + " " + target + SATDUMP_DIFF_CARGO + " test --release --offline --manifest-path " +
               SATDUMP_DIFF_CARGO_TOML + " --test difftest replay_a_reproducer";
    }

    Options options_from_env()
    {
        Options o;
        const char *f = std::getenv("SD_DIFF_FAILURES");
        o.failures = f != nullptr && f[0] != '\0' ? f : SATDUMP_DIFF_FAILURES;
        o.max_saved = int(std::min<uint64_t>(env_number("SD_DIFF_MAX_SAVED", 3), 1000));
        return o;
    }

    Plan plan_from_env()
    {
        Plan p;
        p.seed = env_number("SD_DIFF_SEED", p.seed);
        p.cases = env_number("SD_DIFF_CASES", p.cases);
        return p;
    }

    const std::vector<Twin> &all_twins()
    {
        static const std::vector<Twin> twins = []
        {
            std::vector<Twin> t;
            add_derand_twins(t);
            add_selftest_twins(t);
            // Fork part "aos"
            add_aos_twins(t);
            return t;
        }();
        return twins;
    }

    const Twin *find_twin(const std::string &name)
    {
        for (const Twin &t : all_twins())
            if (t.name == name)
                return &t;
        return nullptr;
    }

    Result run(const Twin &twin, const Plan &plan, const Options &opt)
    {
        need_counterpart(twin);
        // One case at a time, so a deep run needs no more memory than a short one
        Result r;
        const uint64_t n = twin.count(plan);
        for (uint64_t i = 0; i < n; i++)
            check(r, twin, twin.make(plan, i), opt);
        return r;
    }

    Result run_cases(const Twin &twin, const std::vector<Case> &cases, const Options &opt)
    {
        need_counterpart(twin);
        Result r;
        for (const Case &c : cases)
            check(r, twin, c, opt);
        return r;
    }

    std::vector<Case> corpus_cases(const Twin &twin, const std::string &path, const std::string &relative_to)
    {
        std::vector<fs::path> files;
        if (fs::is_directory(path))
        {
            for (const auto &e : fs::directory_iterator(path))
                if (e.is_regular_file())
                    files.push_back(e.path());
            std::sort(files.begin(), files.end());
        }
        else
            files.push_back(path);

        std::vector<Case> out;
        for (const fs::path &file : files)
        {
            std::ifstream f(file, std::ios::binary);
            if (!f)
                throw std::runtime_error("cannot read " + file.string());
            const std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            auto from_corpus = [&](size_t at, size_t length)
            {
                Case c;
                c.id = file.filename().string() + "@" + std::to_string(at);
                c.corpus = relative_to.empty() ? fs::absolute(file).string()
                                               : fs::relative(fs::absolute(file), fs::absolute(relative_to)).string();
                c.at = at;
                c.length = length;
                c.input.assign(data.begin() + at, data.begin() + at + length);
                return c;
            };
            if (twin.stream)
            {
                // The whole file, fed frame by frame as the file readers do
                Case c = from_corpus(0, data.size());
                c.chunks = fixed_chunks(data.size(), twin.corpus_frame ? twin.corpus_frame : data.size());
                out.push_back(c);
                continue;
            }
            // Whole frames, then what is left at the end (a fuzz input is often shorter than a frame)
            const size_t frame = twin.corpus_frame ? twin.corpus_frame : data.size();
            for (size_t at = 0; at < data.size(); at += frame)
            {
                const size_t n = std::min(frame, data.size() - at);
                if (n > twin.corpus_skip)
                    out.push_back(from_corpus(at + twin.corpus_skip, n - twin.corpus_skip));
            }
        }
        return out;
    }

    Result check_oracle(const std::string &path, const Options &opt, const Twin *only)
    {
        std::vector<fs::path> files;
        if (fs::is_directory(path))
        {
            for (const auto &e : fs::directory_iterator(path))
                if (e.path().extension() == ".jsonl")
                    files.push_back(e.path());
            std::sort(files.begin(), files.end());
        }
        else
            files.push_back(path);
        if (files.empty())
            throw std::runtime_error("no .jsonl oracle files in " + path);

        Result r;
        for (const fs::path &file : files)
        {
            std::ifstream f(file);
            if (!f)
                throw std::runtime_error("cannot read " + file.string());
            const uint64_t differences = r.differences, cases = r.cases;
            std::string line;
            for (int n = 1; std::getline(f, line); n++)
            {
                if (line.empty())
                    continue;
                const std::string source = "oracle " + file.filename().string() + ":" + std::to_string(n);
                OracleLine o;
                try
                {
                    o = parse_oracle_line(line, file.parent_path().string());
                }
                catch (const std::exception &e)
                {
                    throw std::runtime_error(file.string() + ":" + std::to_string(n) + ": " + e.what());
                }
                const Twin *twin = only != nullptr ? only : find_twin(o.twin);
                if (twin == nullptr)
                    throw std::runtime_error(source + ": this build has no twin " + o.twin);
                if (o.floats.max_ulps != twin->floats.max_ulps)
                    throw std::runtime_error(source + ": the twin's float rule changed; write this oracle again");
                r.cases++;
                // Is the oracle still what the C++ gives? Then does the counterpart give it too?
                const Outcome cpp = run_side(twin->cpp, o.c, true);
                std::string d = differ(o.out, cpp, o.floats, "the oracle", "C++");
                if (!d.empty())
                    note(r, opt, *twin, o.c, o.out, cpp, "the oracle", "C++", "C++ changed since the oracle was written: " + d, source);
                if (twin->rust)
                {
                    const Outcome rs = run_side(twin->rust, o.c, false);
                    d = differ(o.out, rs, o.floats, "the oracle", twin->rust_name);
                    if (!d.empty())
                        note(r, opt, *twin, o.c, o.out, rs, "the oracle", twin->rust_name, d, source);
                }
            }
            std::cout << "diff: " << file.string() << ": " << (r.cases - cases) << " cases, "
                      << (r.differences - differences) << " differences" << std::endl;
            // An emptied or cut oracle file would otherwise check nothing and pass
            if (r.cases - cases < opt.min_cases)
                throw std::runtime_error(file.string() + " has " + std::to_string(r.cases - cases) + " cases, fewer than " +
                                         std::to_string(opt.min_cases));
        }
        return r;
    }

    Result replay(const std::string &folder, const Twin *twin)
    {
        const Saved s = load_reproducer(folder);
        if (twin == nullptr)
            twin = find_twin(s.twin);
        if (twin == nullptr)
            throw std::runtime_error("this build has no twin " + s.twin);

        Result r;
        r.cases = 1;
        auto count = [&r](const std::string &d)
        {
            if (d.empty())
                return;
            r.differences++;
            if (r.first.empty())
                r.first = d;
        };
        const Outcome cpp = run_side(twin->cpp, s.c, true);
        if (twin->rust)
        {
            const std::string d = differ(cpp, run_side(twin->rust, s.c, false), twin->floats, "C++", twin->rust_name);
            std::cout << "replay " << folder << ": " << twin->name << " case " << s.c.id << ": "
                      << (d.empty() ? "C++ and " + twin->rust_name + " agree" : d) << std::endl;
            count(d);
        }
        // A case saved from an oracle: does the C++ still give what the oracle says?
        if (s.source != "live")
        {
            const std::string d = differ(s.expected, cpp, twin->floats, "the oracle", "C++");
            std::cout << "replay " << folder << ": the saved oracle outcome and C++ "
                      << (d.empty() ? "agree" : "differ: " + d) << std::endl;
            count(d);
        }
        return r;
    }
} // namespace satdump::diff
