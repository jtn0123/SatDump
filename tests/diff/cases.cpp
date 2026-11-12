// Fork: seeded cases, fill styles, mutations and chunking for the differential tests (diff.h).
// rust/tests/difftest/rng.rs must give the same bytes: change both together.
#include "diff.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace satdump::diff
{
    Rng Rng::for_case(uint64_t seed, uint64_t index)
    {
        return Rng(seed ^ (index * 0xD1B54A32D192ED03ULL));
    }

    uint64_t Rng::next()
    {
        state += 0x9E3779B97F4A7C15ULL;
        uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }

    uint64_t Rng::below(uint64_t n)
    {
        return n == 0 ? 0 : next() % n;
    }

    uint64_t fnv1a(const uint8_t *data, size_t size, uint64_t h)
    {
        for (size_t i = 0; i < size; i++)
        {
            h ^= data[i];
            h *= 0x100000001B3ULL;
        }
        return h;
    }

    std::vector<uint8_t> fill(uint64_t seed, uint64_t index, size_t length, Style style)
    {
        static const uint8_t extremes[6] = {0x00, 0x01, 0x7F, 0x80, 0xFE, 0xFF};
        std::vector<uint8_t> out(length, style == Style::Ones ? 0xFF : 0x00);
        if (style == Style::Zero || style == Style::Ones)
            return out;
        Rng rng = Rng::for_case(seed, index);
        uint64_t word = 0;
        for (size_t i = 0; i < length; i++)
        {
            // Eight bytes from each number, lowest first
            if (i % 8 == 0)
                word = rng.next();
            const uint8_t b = uint8_t(word >> (8 * (i % 8)));
            if (style == Style::Bits)
                out[i] = b & 1;
            else if (style == Style::Extremes)
                out[i] = extremes[b % 6];
            else if (style == Style::Sparse)
                out[i] = (b & 0xF0) == 0 ? b : 0;
            else
                out[i] = b;
        }
        return out;
    }

    void apply(std::vector<uint8_t> &d, const Mutation &m)
    {
        const uint64_t n = d.size();
        // A range of b bytes at a must lie inside the data
        auto inside = [n](uint64_t a, uint64_t b) { return b <= n && a <= n - b; };
        if (m.op == "flip" && m.a / 8 < n)
            d[m.a / 8] ^= uint8_t(0x80 >> (m.a % 8));
        else if (m.op == "set" && m.a < n && m.b < 256)
            d[m.a] = uint8_t(m.b);
        else if (m.op == "truncate" && m.a <= n)
            d.resize(m.a);
        else if (m.op == "copy" && inside(m.a, m.b) && inside(m.c, m.b))
            std::memmove(d.data() + m.c, d.data() + m.a, m.b);
        else if (m.op == "cut" && inside(m.a, m.b))
            d.erase(d.begin() + m.a, d.begin() + m.a + m.b);
        else if (m.op == "repeat" && inside(m.a, m.b))
        {
            const std::vector<uint8_t> part(d.begin() + m.a, d.begin() + m.a + m.b);
            d.insert(d.begin() + m.a + m.b, part.begin(), part.end());
        }
        else
            throw std::invalid_argument("bad mutation " + m.op + " " + std::to_string(m.a) + " " +
                                        std::to_string(m.b) + " " + std::to_string(m.c) + " on " +
                                        std::to_string(n) + " bytes");
    }

    int64_t Case::param(const std::string &name, int64_t fallback) const
    {
        auto it = params.find(name);
        return it == params.end() ? fallback : it->second;
    }

    Case generated(const std::string &id, uint64_t seed, uint64_t index, size_t length, Style style)
    {
        Case c;
        c.id = id;
        c.seed = seed;
        c.index = index;
        c.length = length;
        c.style = style;
        c.input = fill(seed, index, length, style);
        return c;
    }

    namespace
    {
        void change(Case &c, const Mutation &m)
        {
            apply(c.input, m);
            c.mutations.push_back(m);
        }

        uint64_t chunk_start(const Case &c, size_t k)
        {
            uint64_t pos = 0;
            for (size_t i = 0; i < k; i++)
                pos += c.chunks[i];
            return pos;
        }
    } // namespace

    void flip_bit(Case &c, uint64_t bit)
    {
        change(c, {"flip", bit, 0, 0});
    }

    void set_byte(Case &c, uint64_t pos, uint8_t value)
    {
        change(c, {"set", pos, value, 0});
    }

    void truncate(Case &c, uint64_t length)
    {
        change(c, {"truncate", length, 0, 0});
        // The chunks keep their sizes up to the new end
        uint64_t left = length;
        std::vector<size_t> kept;
        for (size_t n : c.chunks)
        {
            if (left == 0)
                break;
            kept.push_back(size_t(std::min<uint64_t>(n, left)));
            left -= kept.back();
        }
        if (!c.chunks.empty())
            c.chunks = kept;
    }

    void splice(Case &c, uint64_t from, uint64_t length, uint64_t to)
    {
        change(c, {"copy", from, length, to});
    }

    void drop_chunk(Case &c, size_t k)
    {
        change(c, {"cut", chunk_start(c, k), c.chunks.at(k), 0});
        c.chunks.erase(c.chunks.begin() + k);
    }

    void dup_chunk(Case &c, size_t k)
    {
        change(c, {"repeat", chunk_start(c, k), c.chunks.at(k), 0});
        c.chunks.insert(c.chunks.begin() + k + 1, c.chunks[k]);
    }

    void mutate(Case &c, Rng &rng, int count)
    {
        for (int i = 0; i < count; i++)
        {
            const uint64_t n = c.input.size();
            if (n == 0)
                return;
            switch (rng.below(c.chunks.size() > 1 ? 7 : 5))
            {
            case 0:
            case 1: // bit errors are what a weak signal gives most
                flip_bit(c, rng.below(n * 8));
                break;
            case 2:
                set_byte(c, rng.below(n), uint8_t(rng.next()));
                break;
            case 3:
                truncate(c, rng.below(n));
                break;
            case 4:
            {
                const uint64_t len = 1 + rng.below(std::min<uint64_t>(n / 2 + 1, 64));
                if (len <= n)
                    splice(c, rng.below(n - len + 1), len, rng.below(n - len + 1));
                break;
            }
            case 5:
                drop_chunk(c, size_t(rng.below(c.chunks.size())));
                break;
            default:
                dup_chunk(c, size_t(rng.below(c.chunks.size())));
                break;
            }
        }
    }

    std::vector<size_t> chunks_of(const Case &c)
    {
        if (c.chunks.empty())
            return {c.input.size()};
        size_t sum = 0;
        for (size_t n : c.chunks)
            sum += n;
        if (sum != c.input.size())
            throw std::invalid_argument("case " + c.id + ": the chunks add up to " + std::to_string(sum) + " bytes, not " +
                                        std::to_string(c.input.size()));
        return c.chunks;
    }

    std::vector<size_t> fixed_chunks(size_t total, size_t size)
    {
        size = std::max<size_t>(size, 1);
        std::vector<size_t> out;
        for (size_t pos = 0; pos < total; pos += size)
            out.push_back(std::min(size, total - pos));
        return out;
    }

    std::vector<size_t> random_chunks(Rng &rng, size_t total, size_t max_chunk, bool empty_chunks)
    {
        max_chunk = std::max<size_t>(max_chunk, 1);
        std::vector<size_t> out;
        while (total > 0)
        {
            size_t n = empty_chunks ? size_t(rng.below(max_chunk + 1)) : size_t(1 + rng.below(max_chunk));
            n = std::min(n, total);
            out.push_back(n);
            total -= n;
        }
        return out;
    }

    size_t pick_length(Rng &rng, size_t max, const std::vector<size_t> &near)
    {
        switch (rng.below(4))
        {
        case 0:
            return size_t(rng.below(std::min<size_t>(max, 64) + 1));
        case 1:
            return size_t(rng.below(max + 1));
        case 2:
            if (!near.empty())
            {
                const uint64_t base = near[rng.below(near.size())] * (1 + rng.below(4));
                const uint64_t v = base + rng.below(5);
                return size_t(std::min<uint64_t>(v < 2 ? 0 : v - 2, max));
            }
            return size_t(rng.below(max + 1));
        default:
            return max - size_t(rng.below(std::min<size_t>(max, 16) + 1));
        }
    }
} // namespace satdump::diff
