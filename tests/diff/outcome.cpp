// Fork: outcomes of the differential tests (diff.h): digests, float distances and the first
// difference in plain words. rust/tests/difftest/outcome.rs computes the same digests.
#include "diff.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace satdump::diff
{
    namespace
    {
        uint64_t fnv_u64(uint64_t h, uint64_t v)
        {
            uint8_t le[8];
            for (int i = 0; i < 8; i++)
                le[i] = uint8_t(v >> (8 * i));
            return fnv1a(le, 8, h);
        }

        char kind_char(Value::Kind k)
        {
            switch (k)
            {
            case Value::Kind::Int:
                return 'i';
            case Value::Kind::Bytes:
                return 'b';
            case Value::Kind::F32:
                return 'f';
            default:
                return 'd';
            }
        }

        const char *kind_name(Value::Kind k)
        {
            switch (k)
            {
            case Value::Kind::Int:
                return "an integer";
            case Value::Kind::Bytes:
                return "bytes";
            case Value::Kind::F32:
                return "32-bit floats";
            default:
                return "64-bit floats";
            }
        }

        // Floats as unsigned numbers in the same order as the values: -0 and +0 meet in the middle
        uint64_t ordered(uint64_t bits, int width)
        {
            const uint64_t sign = 1ULL << (width - 1);
            const uint64_t magnitude = bits & (sign - 1);
            return (bits & sign) ? (1ULL << 63) - magnitude : (1ULL << 63) + magnitude;
        }

        uint64_t distance(uint64_t a, uint64_t b, int width, bool a_nan, bool b_nan, bool a_inf, bool b_inf)
        {
            if (a_nan || b_nan)
                return a_nan && b_nan ? 0 : NOT_CLOSE;
            if (a_inf || b_inf)
                return a == b ? 0 : NOT_CLOSE;
            const uint64_t oa = ordered(a, width), ob = ordered(b, width);
            return oa > ob ? oa - ob : ob - oa;
        }

        const char *DIGEST_ONLY = " (only a digest was kept; satdump-diff replay shows the first difference)";

        std::string hex64(uint64_t v)
        {
            char s[24];
            std::snprintf(s, sizeof(s), "%016llx", (unsigned long long)v);
            return s;
        }

        std::string hex_byte(uint8_t v)
        {
            char s[8];
            std::snprintf(s, sizeof(s), "0x%02x", v);
            return s;
        }

        // A few bytes on each side of position i, the one at i in brackets
        std::string around(const std::vector<uint8_t> &d, size_t i)
        {
            std::string s;
            const size_t from = i < 4 ? 0 : i - 4, to = std::min(d.size(), i + 5);
            for (size_t k = from; k < to; k++)
            {
                char b[8];
                std::snprintf(b, sizeof(b), k == i ? "[%02x]" : "%02x", d[k]);
                s += (k == from ? "" : " ") + std::string(b);
            }
            return s;
        }

        std::string float_text(uint64_t bits, bool wide)
        {
            char s[64];
            if (wide)
            {
                double v;
                std::memcpy(&v, &bits, 8);
                std::snprintf(s, sizeof(s), "%.17g (0x%016llx)", v, (unsigned long long)bits);
            }
            else
            {
                const uint32_t b32 = uint32_t(bits);
                float v;
                std::memcpy(&v, &b32, 4);
                std::snprintf(s, sizeof(s), "%.9g (0x%08x)", double(v), b32);
            }
            return s;
        }

        std::string compare_floats(const std::string &what, const Value &w, const Value &g, const FloatRule &rule,
                                   const std::string &wn, const std::string &gn)
        {
            const bool wide = w.kind == Value::Kind::F64;
            const size_t n = wide ? w.f64.size() : w.f32.size(), m = wide ? g.f64.size() : g.f32.size();
            for (size_t i = 0; i < std::min(n, m); i++)
            {
                const uint64_t a = wide ? w.f64[i] : w.f32[i], b = wide ? g.f64[i] : g.f32[i];
                if (a == b)
                    continue;
                uint64_t d;
                if (wide)
                {
                    double x, y;
                    std::memcpy(&x, &a, 8), std::memcpy(&y, &b, 8);
                    d = ulp_distance(x, y);
                }
                else
                {
                    const uint32_t a32 = uint32_t(a), b32 = uint32_t(b);
                    float x, y;
                    std::memcpy(&x, &a32, 4), std::memcpy(&y, &b32, 4);
                    d = ulp_distance(x, y);
                }
                if (rule.max_ulps > 0 && d <= rule.max_ulps)
                    continue;
                const std::string apart = d == NOT_CLOSE ? "not comparable (NaN or infinity)"
                                          : d == 0       ? "equal as numbers but not bit for bit (+0 and -0, or two NaNs)"
                                                         : std::to_string(d) + " ULP apart";
                return what + ": value " + std::to_string(i) + " of " + std::to_string(n) + ": " + wn + " " +
                       float_text(a, wide) + ", " + gn + " " + float_text(b, wide) + ", " + apart + ", " +
                       std::to_string(rule.max_ulps) + " allowed";
            }
            if (n != m)
                return what + ": " + wn + " has " + std::to_string(n) + " values, " + gn + " " + std::to_string(m) +
                       " (the same up to there)";
            return "";
        }

        std::string compare_value(const std::string &what, const Value &w, const Value &g, const FloatRule &rule,
                                  const std::string &wn, const std::string &gn)
        {
            if (w.kind != g.kind)
                return what + ": " + wn + " gives " + kind_name(w.kind) + ", " + gn + " " + kind_name(g.kind);
            if (w.kind == Value::Kind::Int)
                return w.i == g.i ? "" : what + ": " + wn + " " + std::to_string(w.i) + ", " + gn + " " + std::to_string(g.i);
            if (w.summary || g.summary)
            {
                if (w.len() == g.len() && w.fnv() == g.fnv())
                    return "";
                if (w.kind != Value::Kind::Bytes && rule.max_ulps > 0)
                    return what + ": a float tolerance needs the values, but only a digest was kept";
                return what + ": " + wn + " " + std::to_string(w.len()) + " long with digest " + hex64(w.fnv()) + ", " +
                       gn + " " + std::to_string(g.len()) + " long with digest " + hex64(g.fnv()) + DIGEST_ONLY;
            }
            if (w.kind != Value::Kind::Bytes)
                return compare_floats(what, w, g, rule, wn, gn);
            const std::vector<uint8_t> &a = w.bytes, &b = g.bytes;
            const auto mm = std::mismatch(a.begin(), a.begin() + std::min(a.size(), b.size()), b.begin());
            const size_t i = size_t(mm.first - a.begin());
            if (i < std::min(a.size(), b.size()))
                return what + ": first difference at byte " + std::to_string(i) + " of " + std::to_string(a.size()) +
                       ": " + wn + " " + hex_byte(a[i]) + ", " + gn + " " + hex_byte(b[i]) + " (" + wn + " " +
                       around(a, i) + ", " + gn + " " + around(b, i) + ")";
            if (a.size() != b.size())
                return what + ": " + wn + " gives " + std::to_string(a.size()) + " bytes, " + gn + " " +
                       std::to_string(b.size()) + " (the same up to there)";
            return "";
        }

        std::string record_names(const Record &r)
        {
            std::string s;
            for (const Field &f : r)
                s += (s.empty() ? "" : ", ") + f.name;
            return "[" + s + "]";
        }
    } // namespace

    uint64_t Value::len() const
    {
        if (summary)
            return length;
        switch (kind)
        {
        case Kind::Bytes:
            return bytes.size();
        case Kind::F32:
            return f32.size();
        case Kind::F64:
            return f64.size();
        default:
            return 0;
        }
    }

    uint64_t Value::fnv() const
    {
        if (summary)
            return digest;
        uint64_t h = FNV_OFFSET;
        if (kind == Kind::Int)
            return fnv_u64(h, uint64_t(i));
        if (kind == Kind::Bytes)
            return fnv1a(bytes.data(), bytes.size(), h);
        for (uint32_t v : f32)
        {
            const uint8_t le[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
            h = fnv1a(le, 4, h);
        }
        for (uint64_t v : f64)
            h = fnv_u64(h, v);
        return h;
    }

    Value Value::integer(int64_t v)
    {
        Value x;
        x.i = v;
        return x;
    }

    Value Value::data(std::vector<uint8_t> v)
    {
        Value x;
        x.kind = Kind::Bytes;
        x.bytes = std::move(v);
        return x;
    }

    Value Value::floats(const std::vector<float> &v)
    {
        Value x;
        x.kind = Kind::F32;
        x.f32.resize(v.size());
        if (!v.empty())
            std::memcpy(x.f32.data(), v.data(), v.size() * 4);
        return x;
    }

    Value Value::doubles(const std::vector<double> &v)
    {
        Value x;
        x.kind = Kind::F64;
        x.f64.resize(v.size());
        if (!v.empty())
            std::memcpy(x.f64.data(), v.data(), v.size() * 8);
        return x;
    }

    uint64_t Outcome::count() const
    {
        return records_summary ? records_count : records.size();
    }

    uint64_t Outcome::digest() const
    {
        if (records_summary)
            return records_digest;
        // Each field: its name, a 0, its kind letter, then the integer, or the length and
        // digest of the data; a 0x1E ends each record
        uint64_t h = FNV_OFFSET;
        for (const Record &r : records)
        {
            for (const Field &f : r)
            {
                h = fnv1a((const uint8_t *)f.name.data(), f.name.size(), h);
                const uint8_t tag[2] = {0, uint8_t(kind_char(f.value.kind))};
                h = fnv1a(tag, 2, h);
                if (f.value.kind == Value::Kind::Int)
                    h = fnv_u64(h, uint64_t(f.value.i));
                else
                    h = fnv_u64(fnv_u64(h, f.value.len()), f.value.fnv());
            }
            const uint8_t end = 0x1E;
            h = fnv1a(&end, 1, h);
        }
        return h;
    }

    FloatRule FloatRule::ulps(uint64_t n, const std::string &why)
    {
        FloatRule r;
        r.max_ulps = n;
        r.reason = why;
        return r;
    }

    uint64_t ulp_distance(float a, float b)
    {
        uint32_t x, y;
        std::memcpy(&x, &a, 4), std::memcpy(&y, &b, 4);
        return distance(x, y, 32, std::isnan(a), std::isnan(b), std::isinf(a), std::isinf(b));
    }

    uint64_t ulp_distance(double a, double b)
    {
        uint64_t x, y;
        std::memcpy(&x, &a, 8), std::memcpy(&y, &b, 8);
        return distance(x, y, 64, std::isnan(a), std::isnan(b), std::isinf(a), std::isinf(b));
    }

    std::string compare(const Outcome &want, const Outcome &got, const FloatRule &rule, const std::string &wn,
                        const std::string &gn)
    {
        if (want.status != got.status)
            return "status: " + wn + " " + std::to_string(want.status) + ", " + gn + " " + std::to_string(got.status);
        std::string d = compare_value("bytes", want.bytes, got.bytes, rule, wn, gn);
        if (!d.empty())
            return d;
        if (want.records_summary || got.records_summary)
        {
            if (want.count() == got.count() && want.digest() == got.digest())
                return "";
            return "records: " + wn + " has " + std::to_string(want.count()) + " with digest " + hex64(want.digest()) +
                   ", " + gn + " " + std::to_string(got.count()) + " with digest " + hex64(got.digest()) + DIGEST_ONLY;
        }
        const size_t n = want.records.size(), m = got.records.size();
        for (size_t k = 0; k < std::min(n, m); k++)
        {
            const Record &a = want.records[k], &b = got.records[k];
            const std::string where = "record " + std::to_string(k);
            for (size_t f = 0; f < std::min(a.size(), b.size()); f++)
            {
                if (a[f].name != b[f].name)
                    return where + ": fields " + wn + " " + record_names(a) + ", " + gn + " " + record_names(b);
                d = compare_value(where + " " + a[f].name, a[f].value, b[f].value, rule, wn, gn);
                if (!d.empty())
                    return d;
            }
            if (a.size() != b.size())
                return where + ": fields " + wn + " " + record_names(a) + ", " + gn + " " + record_names(b);
        }
        if (n != m)
            return "records: " + wn + " gives " + std::to_string(n) + ", " + gn + " " + std::to_string(m) +
                   " (the same up to there; the first extra one has fields " +
                   record_names(n > m ? want.records[m] : got.records[n]) + ")";
        return "";
    }
} // namespace satdump::diff
