// Fork: JSONL oracles and reproducers for the differential tests (diff.h). The Rust side reads
// and writes the same layout (rust/tests/difftest/oracle.rs).
//
// An oracle line: {"twin": name, "case": case, "floats": {"max_ulps", "reason"}, "out": outcome}
//   case:    {"id", "seed" (16 hex digits), "index", "length", "style", "corpus", "at",
//             "mutations": [[op, a, b, c]...], "chunks": [...], "params": {...},
//             "size" and "fnv" of the input bytes, and "input" (hex) when the case carries
//             its bytes (inline_input; the description is then not used to make them again)}
//   outcome: {"status", "bytes": value, "records": {"count", "fnv", "list": [record...]}}
//   record:  [[name, value]...] in order
//   value:   an integer, or {"bytes"|"f32"|"f64": length, "fnv", "hex"|"bits"} where "hex"
//            (bytes) or "bits" (floats, as hex bit patterns) is left out for long data
// A reproducer folder: input.bin, case.json ({"twin", "case", "floats", "difference",
// "source", "expected", "actual", "replay", "replay_rust"}), expected.json and actual.json
// (outcomes).
#include "diff.h"
#include "nlohmann/json.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace fs = std::filesystem;
using nlohmann::json;

namespace satdump::diff
{
    namespace
    {
        constexpr size_t RECORDS_INLINE = 256; // records kept in full in an oracle line, at most (but see outcome_json)

        std::string hex64(uint64_t v)
        {
            char s[24];
            std::snprintf(s, sizeof(s), "%016llx", (unsigned long long)v);
            return s;
        }

        uint64_t unhex64(const std::string &s)
        {
            size_t used = 0;
            const uint64_t v = std::stoull(s, &used, 16);
            if (used != s.size() || s.empty())
                throw std::invalid_argument("not a hex number: " + s);
            return v;
        }

        std::string hex(const uint8_t *d, size_t n)
        {
            static const char digits[] = "0123456789abcdef";
            std::string s(2 * n, '0');
            for (size_t i = 0; i < n; i++)
                s[2 * i] = digits[d[i] >> 4], s[2 * i + 1] = digits[d[i] & 15];
            return s;
        }

        std::vector<uint8_t> unhex(const std::string &s)
        {
            if (s.size() % 2)
                throw std::invalid_argument("odd hex length");
            std::vector<uint8_t> out(s.size() / 2);
            for (size_t i = 0; i < out.size(); i++)
                out[i] = uint8_t(unhex64(s.substr(2 * i, 2)));
            return out;
        }

        json value_json(const Value &v, size_t inline_max, bool keep_floats)
        {
            if (v.kind == Value::Kind::Int)
                return v.i;
            const bool bytes = v.kind == Value::Kind::Bytes, wide = v.kind == Value::Kind::F64;
            json o;
            o[bytes ? "bytes" : wide ? "f64" : "f32"] = v.len();
            o["fnv"] = hex64(v.fnv());
            const uint64_t size = v.len() * (bytes ? 1 : wide ? 8 : 4);
            if (v.summary || (size > inline_max && !(keep_floats && !bytes)))
                return o;
            if (bytes)
                o["hex"] = hex(v.bytes.data(), v.bytes.size());
            else
            {
                json bits = json::array();
                for (size_t i = 0; i < v.len(); i++)
                {
                    char s[24];
                    if (wide)
                        std::snprintf(s, sizeof(s), "%016llx", (unsigned long long)v.f64[i]);
                    else
                        std::snprintf(s, sizeof(s), "%08x", (unsigned)v.f32[i]);
                    bits.push_back(s);
                }
                o["bits"] = bits;
            }
            return o;
        }

        Value value_from_json(const json &j)
        {
            if (j.is_number_integer())
                return Value::integer(j.get<int64_t>());
            Value v;
            if (j.contains("bytes"))
                v.kind = Value::Kind::Bytes, v.length = j["bytes"].get<uint64_t>();
            else if (j.contains("f32"))
                v.kind = Value::Kind::F32, v.length = j["f32"].get<uint64_t>();
            else if (j.contains("f64"))
                v.kind = Value::Kind::F64, v.length = j["f64"].get<uint64_t>();
            else
                throw std::invalid_argument("not a value: " + j.dump());
            v.digest = unhex64(j.at("fnv").get<std::string>());
            if (j.contains("hex"))
                v.bytes = unhex(j["hex"].get<std::string>());
            else if (j.contains("bits"))
                for (const json &b : j["bits"])
                    if (v.kind == Value::Kind::F32)
                        v.f32.push_back(uint32_t(unhex64(b.get<std::string>())));
                    else
                        v.f64.push_back(unhex64(b.get<std::string>()));
            else
            {
                v.summary = true;
                return v;
            }
            if (v.len() != v.length || v.fnv() != v.digest)
                throw std::invalid_argument("a value's data does not match its length and digest: " + j.dump());
            return v;
        }

        json outcome_json(const Outcome &o, size_t inline_max, bool keep_floats)
        {
            json records;
            records["count"] = o.count();
            records["fnv"] = hex64(o.digest());
            // All of them when a float tolerance needs the values to compare
            if (!o.records_summary && (o.records.size() <= RECORDS_INLINE || inline_max == SIZE_MAX || keep_floats))
            {
                json list = json::array();
                for (const Record &r : o.records)
                {
                    json fields = json::array();
                    for (const Field &f : r)
                        fields.push_back(json::array({f.name, value_json(f.value, inline_max, keep_floats)}));
                    list.push_back(fields);
                }
                records["list"] = list;
            }
            return {{"status", o.status}, {"bytes", value_json(o.bytes, inline_max, keep_floats)}, {"records", records}};
        }

        Outcome outcome_from_json(const json &j)
        {
            Outcome o;
            o.status = j.at("status").get<int64_t>();
            o.bytes = value_from_json(j.at("bytes"));
            const json &r = j.at("records");
            o.records_count = r.at("count").get<uint64_t>();
            o.records_digest = unhex64(r.at("fnv").get<std::string>());
            if (!r.contains("list"))
            {
                o.records_summary = true;
                return o;
            }
            for (const json &rec : r["list"])
            {
                Record fields;
                for (const json &f : rec)
                    fields.push_back({f.at(0).get<std::string>(), value_from_json(f.at(1))});
                o.records.push_back(fields);
            }
            if (o.count() != o.records_count || o.digest() != o.records_digest)
                throw std::invalid_argument("the records do not match their count and digest");
            return o;
        }

        json case_json(const Case &c)
        {
            json muts = json::array();
            for (const Mutation &m : c.mutations)
                muts.push_back(json::array({m.op, m.a, m.b, m.c}));
            json j = {{"id", c.id}, {"seed", hex64(c.seed)}, {"index", c.index}, {"length", c.length},
                      {"style", int(c.style)}, {"corpus", c.corpus}, {"at", c.at}, {"mutations", muts},
                      {"chunks", c.chunks}, {"params", c.params}, {"size", c.input.size()},
                      {"fnv", hex64(fnv1a(c.input.data(), c.input.size()))}};
            if (c.inline_input)
                j["input"] = hex(c.input.data(), c.input.size());
            return j;
        }

        Case case_from_json(const json &j)
        {
            Case c;
            c.id = j.at("id").get<std::string>();
            c.seed = unhex64(j.at("seed").get<std::string>());
            c.index = j.at("index").get<uint64_t>();
            c.length = j.at("length").get<uint64_t>();
            const int style = j.at("style").get<int>();
            if (style < 0 || style >= int(Style::Count))
                throw std::invalid_argument("unknown fill style " + std::to_string(style));
            c.style = Style(style);
            c.corpus = j.at("corpus").get<std::string>();
            c.at = j.at("at").get<uint64_t>();
            for (const json &m : j.at("mutations"))
                c.mutations.push_back({m.at(0).get<std::string>(), m.at(1).get<uint64_t>(), m.at(2).get<uint64_t>(),
                                       m.at(3).get<uint64_t>()});
            c.chunks = j.at("chunks").get<std::vector<size_t>>();
            c.params = j.at("params").get<std::map<std::string, int64_t>>();
            c.inline_input = j.contains("input");
            return c;
        }

        std::vector<uint8_t> read_file(const std::string &path, uint64_t at = 0, uint64_t length = UINT64_MAX)
        {
            std::ifstream f(path, std::ios::binary);
            if (!f)
                throw std::runtime_error("cannot read " + path);
            f.seekg(0, std::ios::end);
            const uint64_t size = uint64_t(f.tellg());
            if (at > size)
                throw std::runtime_error(path + " is shorter than " + std::to_string(at) + " bytes");
            length = std::min(length, size - at);
            std::vector<uint8_t> out(length);
            f.seekg(std::streamoff(at));
            f.read((char *)out.data(), std::streamsize(length));
            if (uint64_t(f.gcount()) != length)
                throw std::runtime_error("cannot read " + path);
            return out;
        }

        // The case's bytes, made again from its description
        void rebuild(Case &c, const json &j, const std::string &folder)
        {
            if (c.inline_input)
                c.input = unhex(j.at("input").get<std::string>()); // mutations included
            else
            {
                if (c.corpus.empty())
                    c.input = fill(c.seed, c.index, c.length, c.style);
                else
                {
                    const fs::path p = fs::path(c.corpus).is_absolute() ? fs::path(c.corpus) : fs::path(folder) / c.corpus;
                    c.input = read_file(p.string(), c.at, c.length);
                }
                for (const Mutation &m : c.mutations)
                    apply(c.input, m);
            }
            if (c.input.size() != j.at("size").get<uint64_t>() ||
                hex64(fnv1a(c.input.data(), c.input.size())) != j.at("fnv").get<std::string>())
                throw std::runtime_error("the case generator no longer makes the same bytes for case " + c.id);
        }

        void write_file(const fs::path &p, const std::string &text)
        {
            std::ofstream f(p, std::ios::binary);
            f << text;
            if (!f)
                throw std::runtime_error("cannot write " + p.string());
        }

        std::string safe_name(std::string s)
        {
            for (char &ch : s)
                if (!isalnum((unsigned char)ch) && ch != '-' && ch != '_' && ch != '.')
                    ch = '_';
            return s;
        }
    } // namespace

    std::string oracle_line(const Twin &twin, const Case &c, const Outcome &cpp, size_t inline_max)
    {
        json j = {{"twin", twin.name},
                  {"case", case_json(c)},
                  {"floats", {{"max_ulps", twin.floats.max_ulps}, {"reason", twin.floats.reason}}},
                  {"out", outcome_json(cpp, inline_max, twin.floats.max_ulps > 0)}};
        return j.dump();
    }

    std::string save_reproducer(const Options &opt, const Twin &twin, const Case &c, const Outcome &want,
                                const Outcome &got, const std::string &wn, const std::string &gn,
                                const std::string &difference, const std::string &source)
    {
        const fs::path dir = fs::path(opt.failures) / safe_name(twin.name + "-" + c.id + (source == "live" ? "" : "-" + gn));
        fs::remove_all(dir);
        fs::create_directories(dir);
        write_file(dir / "input.bin", std::string(c.input.begin(), c.input.end()));
        json j = {{"twin", twin.name},
                  {"case", case_json(c)},
                  {"floats", {{"max_ulps", twin.floats.max_ulps}, {"reason", twin.floats.reason}}},
                  {"difference", difference},
                  {"source", source},
                  {"expected", wn},
                  {"actual", gn},
                  {"replay", replay_command(dir.string())},
                  {"replay_rust", rust_replay_command(dir.string())}};
        write_file(dir / "case.json", j.dump(2) + "\n");
        write_file(dir / "expected.json", outcome_json(want, SIZE_MAX, true).dump(1) + "\n");
        write_file(dir / "actual.json", outcome_json(got, SIZE_MAX, true).dump(1) + "\n");
        return dir.string();
    }

    OracleLine parse_oracle_line(const std::string &line, const std::string &folder)
    {
        const json j = json::parse(line);
        OracleLine o;
        o.twin = j.at("twin").get<std::string>();
        o.c = case_from_json(j.at("case"));
        rebuild(o.c, j.at("case"), folder);
        o.floats.max_ulps = j.at("floats").at("max_ulps").get<uint64_t>();
        o.floats.reason = j.at("floats").at("reason").get<std::string>();
        o.out = outcome_from_json(j.at("out"));
        return o;
    }

    Saved load_reproducer(const std::string &folder)
    {
        const fs::path dir(folder);
        std::ifstream f(dir / "case.json");
        if (!f)
            throw std::runtime_error("no case.json in " + folder);
        const json j = json::parse(f);
        Saved s;
        s.twin = j.at("twin").get<std::string>();
        s.c = case_from_json(j.at("case"));
        s.c.input = read_file((dir / "input.bin").string());
        s.source = j.at("source").get<std::string>();
        std::ifstream e(dir / "expected.json");
        if (!e)
            throw std::runtime_error("no expected.json in " + folder);
        s.expected = outcome_from_json(json::parse(e));
        return s;
    }
} // namespace satdump::diff
