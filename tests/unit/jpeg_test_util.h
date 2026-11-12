// Fork: helpers for the JPEG tests (test_jpeg8_decompress.cpp, test_jpeg12.cpp) and the JPEG fuzz
// harnesses: hashes of pictures and streams, a test picture, and small edits of a JPEG stream's
// headers. No libjpeg in here, so it can be used next to either copy (the 8-bit and 12-bit
// headers cannot share a file).
#pragma once

#include "image/image.h"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace jpeg_test
{
    // FNV-1a, 64 bit (the same hash test_jpeg8.cpp uses)
    struct Hash
    {
        uint64_t h = 1469598103934665603ULL;

        void bytes(const void *p, size_t n)
        {
            const uint8_t *b = (const uint8_t *)p;
            for (size_t i = 0; i < n; i++)
            {
                h ^= b[i];
                h *= 1099511628211ULL;
            }
        }

        void u64(uint64_t v) { bytes(&v, sizeof(v)); }
    };

    // Size, depth and every sample of a picture; an empty picture has a hash too
    inline uint64_t image_hash(satdump::image::Image &img)
    {
        Hash hash;
        hash.u64(img.depth());
        hash.u64(img.width());
        hash.u64(img.height());
        hash.u64(img.channels());
        if (img.size() > 0)
            hash.bytes(img.raw_data(), img.size() * img.typesize());
        return hash.h;
    }

    inline uint64_t bytes_hash(const std::vector<uint8_t> &v)
    {
        Hash hash;
        hash.u64(v.size());
        hash.bytes(v.data(), v.size());
        return hash.h;
    }

    // A test picture with samples from 0 to maxval: gradients, sharp edges and noise, different
    // in each component. Samples are interleaved (one pixel's components next to each other).
    inline std::vector<int> test_samples(int width, int height, int components, int maxval)
    {
        std::vector<int> out((size_t)width * height * components);
        uint32_t lcg = 12345u;
        for (int y = 0; y < height; y++)
            for (int x = 0; x < width; x++)
                for (int c = 0; c < components; c++)
                {
                    lcg = lcg * 1103515245u + 12345u;
                    long v = (long)(x * (5 + 3 * c) + y * (2 + 7 * c)) * maxval / (width + height + 1);
                    if (((x / 6) + (y / 5) + c) % 4 == 0)
                        v = maxval - v;
                    v += (long)((lcg >> 16) % 31) * (maxval / 255) - 15 * (maxval / 255);
                    v = v < 0 ? 0 : (v > maxval ? maxval : v);
                    out[((size_t)y * width + x) * components + c] = (int)v;
                }
        return out;
    }

    // Where the marker segment FF <marker> starts in the headers (before the first scan's data),
    // or 0 when there is none
    inline size_t find_marker(const std::vector<uint8_t> &s, uint8_t marker, size_t from = 2)
    {
        size_t i = from;
        while (i + 4 <= s.size())
        {
            if (s[i] != 0xFF)
                return 0;
            uint8_t m = s[i + 1];
            if (m == marker)
                return i;
            if (m == 0xDA || m == 0xD9) // start of scan or end of image: no more headers
                return 0;
            i += 2 + ((size_t)s[i + 2] << 8 | s[i + 3]);
        }
        return 0;
    }

    // The frame header (SOF0, SOF1, SOF2 or SOF3)
    inline size_t find_sof(const std::vector<uint8_t> &s)
    {
        for (uint8_t m : {0xC0, 0xC1, 0xC2, 0xC3})
        {
            size_t at = find_marker(s, m);
            if (at != 0)
                return at;
        }
        return 0;
    }

    inline void put16(std::vector<uint8_t> &s, size_t at, unsigned v)
    {
        s[at] = (uint8_t)(v >> 8);
        s[at + 1] = (uint8_t)v;
    }

    // The same stream claiming another picture size in its frame header
    inline std::vector<uint8_t> with_size(std::vector<uint8_t> s, unsigned width, unsigned height)
    {
        size_t sof = find_sof(s);
        if (sof != 0)
        {
            put16(s, sof + 5, height);
            put16(s, sof + 7, width);
        }
        return s;
    }

    inline std::vector<uint8_t> cut(const std::vector<uint8_t> &s, size_t length)
    {
        return std::vector<uint8_t>(s.begin(), s.begin() + (length < s.size() ? length : s.size()));
    }

    // The offset of the n-th (from 0) start-of-scan marker in a stream, or 0
    inline size_t nth_sos(const std::vector<uint8_t> &s, int n)
    {
        for (size_t i = 2; i + 1 < s.size(); i++)
            if (s[i] == 0xFF && s[i + 1] == 0xDA && n-- == 0)
                return i;
        return 0;
    }

    // Where the picture data of the first scan starts, or 0
    inline size_t first_scan_data(const std::vector<uint8_t> &s)
    {
        size_t sos = nth_sos(s, 0);
        return sos == 0 ? 0 : sos + 2 + ((size_t)s[sos + 2] << 8 | s[sos + 3]);
    }

    // The same stream with every symbol of its first DC Huffman table set to sym
    inline std::vector<uint8_t> with_dc_symbols(std::vector<uint8_t> s, uint8_t sym)
    {
        size_t at = find_marker(s, 0xC4);
        while (at != 0)
        {
            size_t end = at + 2 + ((size_t)s[at + 2] << 8 | s[at + 3]);
            size_t p = at + 4;
            while (p + 17 <= end)
            {
                size_t count = 0;
                for (int i = 1; i <= 16; i++)
                    count += s[p + i];
                if ((s[p] >> 4) == 0)
                {
                    for (size_t i = 0; i < count && p + 17 + i < end; i++)
                        s[p + 17 + i] = sym;
                    return s;
                }
                p += 17 + count;
            }
            at = find_marker(s, 0xC4, end);
        }
        return s;
    }

    // Whether every sample of a picture is 0
    inline bool all_zero(satdump::image::Image &img)
    {
        for (size_t i = 0; i < img.size(); i++)
            if (img.get(i) != 0)
                return false;
        return true;
    }
} // namespace jpeg_test
