// Fork: satdump-diff, the differential tests' command (diff.h). Built only in Rust builds
// with tests; never installed.
#include "diff.h"
#include "logger.h"
#include "rust_bridge.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace
{
    using namespace satdump::diff;
    namespace fs = std::filesystem;

    const char *USAGE = R"(usage: satdump-diff <command>
  list                        the twins
  run [twin...] [--corpus P]  C++ against Rust on every case of each twin (default: all), or on
                              the CADUs/files in P (a file or a folder; may be given again)
  oracle <twin|all> [--small] [--cases N] [--inline-max BYTES] [--corpus P] [--out FILE or FOLDER]
                              what the C++ twin gives, one JSON line per case (--small: the
                              short plan used for the committed oracles in tests/diff/oracles,
                              every output in full; otherwise outputs over 256 bytes are kept
                              as digests; --corpus: the cases of the files in P instead of the
                              plan, their paths relative to the oracle file's folder; all:
                              every twin, each to FOLDER/<twin>.jsonl)
  check [--min-cases N] <oracle.jsonl or folder>...
                              the C++ still gives each oracle line, and Rust gives it too; a
                              file with fewer than N lines (default 1) is an error
  replay <reproducer folder>  run a saved case again (C++ against Rust, and the saved oracle)
Settings: SD_DIFF_SEED (default 20261112), SD_DIFF_CASES seeded cases per twin (default 2000;
a deep run: 200000), both decimal; SD_DIFF_FAILURES (reproducers; default
<build>/diff-failures), SD_DIFF_MAX_SAVED (default 3).
Exit code: 0 all agree, 1 a difference, 2 a usage or set-up error.
)";

    int usage()
    {
        std::cerr << USAGE;
        return 2;
    }

    void need_rust()
    {
        if (!satdump::rust::enabled())
            throw std::runtime_error("Rust is switched off (SATDUMP_NO_RUST); there is nothing to compare");
    }

    double seconds_since(std::chrono::steady_clock::time_point t)
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
    }

    int report(const Result &r, const Options &opt)
    {
        if (r.differences == 0)
            return 0;
        std::cout << "diff: " << r.differences << " differences; reproducers in " << opt.failures << std::endl;
        return 1;
    }

    int run_command(int argc, char **argv, const Options &opt)
    {
        need_rust();
        std::vector<const Twin *> twins;
        std::vector<std::string> corpora;
        for (int i = 2; i < argc; i++)
        {
            if (std::strcmp(argv[i], "--corpus") == 0 && i + 1 < argc)
                corpora.push_back(argv[++i]);
            else if (const Twin *t = find_twin(argv[i]))
                twins.push_back(t);
            else
                throw std::invalid_argument(std::string("no twin ") + argv[i] + " (satdump-diff list)");
        }
        // Every twin; with --corpus, those that read corpus files as frames
        if (twins.empty())
            for (const Twin &t : all_twins())
                if (corpora.empty() || t.corpus_frame != 0)
                    twins.push_back(&t);

        const Plan plan = plan_from_env();
        std::cout << "diff: seed " << plan.seed << ", " << plan.cases << " seeded cases per twin (SD_DIFF_SEED, SD_DIFF_CASES)" << std::endl;
        Result all;
        for (const Twin *t : twins)
        {
            if (!t->rust)
            {
                std::cout << "diff: " << t->name << ": no counterpart yet (oracle only)" << std::endl;
                continue;
            }
            Options o = opt;
            o.max_saved = std::max(0, opt.max_saved - int(all.saved.size()));
            const auto start = std::chrono::steady_clock::now();
            Result r;
            if (corpora.empty())
                r = run(*t, plan, o);
            else
                for (const std::string &p : corpora)
                {
                    const Result one = run_cases(*t, corpus_cases(*t, p), o);
                    r.cases += one.cases, r.differences += one.differences;
                    r.saved.insert(r.saved.end(), one.saved.begin(), one.saved.end());
                }
            std::cout << "diff: " << t->name << ": " << r.cases << " cases, " << r.differences << " differences (C++ against "
                      << t->rust_name << ") in " << seconds_since(start) << " s" << std::endl;
            all.cases += r.cases, all.differences += r.differences;
            all.saved.insert(all.saved.end(), r.saved.begin(), r.saved.end());
        }
        return report(all, opt);
    }

    // The plan's cases, or those of the corpus files
    uint64_t write_oracle(const Twin &twin, const Plan &plan, const std::vector<std::string> &corpora,
                          const std::string &relative_to, size_t inline_max, std::ostream &os)
    {
        uint64_t n = 0;
        auto write = [&](const Case &c)
        {
            os << oracle_line(twin, c, cpp_outcome(twin, c), inline_max) << "\n";
            n++;
        };
        if (corpora.empty())
            for (uint64_t i = 0; i < twin.count(plan); i++)
                write(twin.make(plan, i));
        for (const std::string &p : corpora)
            for (const Case &c : corpus_cases(twin, p, relative_to))
                write(c);
        os.flush();
        if (!os)
            throw std::runtime_error("cannot write the oracle of " + twin.name);
        return n;
    }

    int oracle_command(int argc, char **argv)
    {
        if (argc < 3)
            return usage();
        const bool all = std::strcmp(argv[2], "all") == 0;
        const Twin *twin = all ? nullptr : find_twin(argv[2]);
        if (!all && twin == nullptr)
            throw std::invalid_argument(std::string("no twin ") + argv[2] + " (satdump-diff list)");
        Plan plan = plan_from_env();
        size_t inline_max = 256;
        std::string out;
        std::vector<std::string> corpora;
        bool cases_given = false, inline_given = false;
        for (int i = 3; i < argc; i++)
        {
            const std::string a = argv[i];
            if (a == "--small")
                plan.small = true;
            else if (a == "--cases" && i + 1 < argc)
                plan.cases = std::stoull(argv[++i]), cases_given = true;
            else if (a == "--inline-max" && i + 1 < argc)
                inline_max = std::stoull(argv[++i]), inline_given = true;
            else if (a == "--corpus" && i + 1 < argc)
                corpora.push_back(argv[++i]);
            else if (a == "--out" && i + 1 < argc)
                out = argv[++i];
            else
                return usage();
        }
        // A small plan keeps every output in full, so the Rust replay names the first differing
        // byte or record; a full plan keeps long outputs as digests
        if (plan.small && !cases_given)
            plan.cases = 6;
        if (plan.small && !inline_given)
            inline_max = SIZE_MAX;
        if (!all && out.empty())
        {
            write_oracle(*twin, plan, corpora, "", inline_max, std::cout);
            return 0;
        }
        if (all && out.empty())
            return usage();
        if (all)
            fs::create_directories(out);
        // Corpus paths in an oracle file are relative to its folder (io.cpp reads them so)
        const std::string folder = all ? out : fs::absolute(out).parent_path().string();
        for (const Twin &t : all_twins())
        {
            if ((!all && &t != twin) || (all && !corpora.empty() && t.corpus_frame == 0))
                continue;
            const std::string path = all ? (fs::path(out) / (t.name + ".jsonl")).string() : out;
            std::ofstream file(path);
            if (!file)
                throw std::runtime_error("cannot write " + path);
            const uint64_t n = write_oracle(t, plan, corpora, folder, inline_max, file);
            std::cout << "diff: wrote " << n << " " << t.name << " cases to " << path << std::endl;
        }
        return 0;
    }
} // namespace

int main(int argc, char **argv)
{
    initLogger();
    logger->set_level(slog::LOG_WARN);
    if (argc < 2)
        return usage();
    const std::string command = argv[1];
    try
    {
        const Options opt = options_from_env();
        if (command == "list")
        {
            for (const Twin &t : all_twins())
                std::cout << t.name << (t.rust ? "" : " (oracle only)") << ": " << t.about << std::endl;
            return 0;
        }
        if (command == "run")
            return run_command(argc, argv, opt);
        if (command == "oracle")
            return oracle_command(argc, argv);
        if (command == "check" && argc >= 3)
        {
            need_rust();
            Result all;
            Options o = opt;
            int first = 2;
            if (std::strcmp(argv[2], "--min-cases") == 0 && argc >= 5)
                o.min_cases = std::stoull(argv[3]), first = 4;
            for (int i = first; i < argc; i++)
            {
                o.max_saved = std::max(0, opt.max_saved - int(all.saved.size()));
                const Result r = check_oracle(argv[i], o);
                all.cases += r.cases, all.differences += r.differences;
                all.saved.insert(all.saved.end(), r.saved.begin(), r.saved.end());
            }
            return report(all, opt);
        }
        if (command == "replay" && argc == 3)
        {
            need_rust();
            return replay(argv[2], nullptr).differences == 0 ? 0 : 1;
        }
        return usage();
    }
    catch (const std::exception &e)
    {
        std::cerr << "satdump-diff: " << e.what() << std::endl;
        return 2;
    }
}
