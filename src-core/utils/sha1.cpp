// Fork: see sha1.h. The steps are those of FIPS 180-4, sections 5.1.1 and 6.1.2
#include "sha1.h"
#include <cstddef>

namespace satdump
{
    static uint32_t rotateLeft(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

    std::array<uint32_t, 5> sha1Digest(const std::string &data)
    {
        std::array<uint32_t, 5> h = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};

        // The bytes, then a 1 bit, then 0 bits up to 8 bytes short of a whole 64-byte block, then
        // the length in bits as 8 bytes, the most significant first
        std::string m = data;
        m += (char)0x80;
        while (m.size() % 64 != 56)
            m += (char)0x00;
        uint64_t bits = (uint64_t)data.size() * 8;
        for (int i = 7; i >= 0; i--)
            m += (char)((bits >> (8 * i)) & 0xFF);

        for (size_t block = 0; block < m.size(); block += 64)
        {
            uint32_t w[80];
            for (size_t t = 0; t < 16; t++)
            {
                const unsigned char *p = (const unsigned char *)m.data() + block + 4 * t;
                w[t] = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
            }
            for (size_t t = 16; t < 80; t++)
                w[t] = rotateLeft(w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16], 1);

            uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
            for (size_t t = 0; t < 80; t++)
            {
                uint32_t f, k;
                if (t < 20)
                {
                    f = (b & c) | (~b & d);
                    k = 0x5A827999;
                }
                else if (t < 40)
                {
                    f = b ^ c ^ d;
                    k = 0x6ED9EBA1;
                }
                else if (t < 60)
                {
                    f = (b & c) | (b & d) | (c & d);
                    k = 0x8F1BBCDC;
                }
                else
                {
                    f = b ^ c ^ d;
                    k = 0xCA62C1D6;
                }
                uint32_t next = rotateLeft(a, 5) + f + e + k + w[t];
                e = d;
                d = c;
                c = rotateLeft(b, 30);
                b = a;
                a = next;
            }
            h[0] += a;
            h[1] += b;
            h[2] += c;
            h[3] += d;
            h[4] += e;
        }
        return h;
    }
} // namespace satdump
