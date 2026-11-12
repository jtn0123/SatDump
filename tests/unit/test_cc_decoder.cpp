// The convolutional (Viterbi) decoder, CCDecoder: k = 7, rate 1/2, polynomials 79 and 109,
// as in every SatDump decoder that uses it. Its input is soft symbols (0 to 255, 128 means
// "no idea"), two per bit; its output is one bit per byte.
#include "common/codings/viterbi/cc_decoder.h"
#include "common/codings/viterbi/cc_encoder.h"
#include "doctest.h"
#include <cstdint>
#include <random>
#include <vector>

namespace
{
    typedef std::vector<uint8_t> Bytes;

    const int K = 7;
    const int RATE = 2;
    const std::vector<int> POLYS = {79, 109};
    const int TAIL = K - 1; // bits the decoder reads past the end of each block

    // Random soft symbols, enough for `blocks` blocks of `frame` bits that follow each other
    Bytes random_softs(int frame, int blocks, uint32_t seed)
    {
        std::mt19937 rng(seed);
        Bytes s((frame * blocks + TAIL) * RATE);
        for (auto &v : s)
            v = uint8_t(rng() & 0xFF);
        return s;
    }

    // Random bits through the encoder, as soft symbols: a 1 is near 255, a 0 near 0, and
    // one symbol in `erase_every` is lost (128). Noise 0 gives clean symbols.
    Bytes coded_softs(const Bytes &bits, int noise, int erase_every, uint32_t seed)
    {
        viterbi::CCEncoder enc(bits.size(), K, RATE, POLYS);
        Bytes coded(bits.size() * RATE);
        enc.work((uint8_t *)bits.data(), coded.data());

        std::mt19937 rng(seed);
        Bytes s(coded.size());
        for (size_t i = 0; i < coded.size(); i++)
        {
            int v = coded[i] ? 255 : 0;
            if (noise > 0)
                v += int(rng() % (2 * noise + 1)) - noise;
            s[i] = uint8_t(v < 0 ? 0 : (v > 255 ? 255 : v));
            if (erase_every > 0 && i % erase_every == 0)
                s[i] = 128;
        }
        return s;
    }

    Bytes random_bits(size_t n, uint32_t seed)
    {
        std::mt19937 rng(seed);
        Bytes b(n);
        for (auto &v : b)
            v = uint8_t(rng() & 1);
        return b;
    }

    // Decodes `blocks` blocks one after the other, as the decoders do: each block starts
    // `frame` bits after the last, and the decoder carries its state from one to the next
    Bytes decode(Bytes softs, int frame, int blocks)
    {
        viterbi::CCDecoder dec(frame, K, RATE, POLYS);
        Bytes out(frame * blocks);
        for (int b = 0; b < blocks; b++)
            dec.work(softs.data() + b * frame * RATE, out.data() + b * frame);
        return out;
    }

    uint64_t fnv1a(const Bytes &v)
    {
        uint64_t h = 0xcbf29ce484222325ULL;
        for (uint8_t c : v)
        {
            h ^= c;
            h *= 0x100000001b3ULL;
        }
        return h;
    }
} // namespace

TEST_CASE("CCDecoder: clean coded bits decode back to the bits that were sent")
{
    const int frame = 2048;
    Bytes bits = random_bits(frame * 4 + TAIL, 41);
    Bytes softs = coded_softs(bits, 0, 0, 0);
    Bytes out = decode(softs, frame, 4);
    CHECK(out == Bytes(bits.begin(), bits.begin() + frame * 4));
}

TEST_CASE("CCDecoder: noisy coded bits with lost symbols still decode to the bits that were sent")
{
    const int frame = 2048;
    Bytes bits = random_bits(frame * 4 + TAIL, 42);
    Bytes softs = coded_softs(bits, 100, 9, 43);
    Bytes out = decode(softs, frame, 4);
    CHECK(out == Bytes(bits.begin(), bits.begin() + frame * 4));
}

// The fork removed the deprecated work(in, out, size), whose size argument was never used.
// Before that, a test compared it with work(in, out) on these inputs and on more sizes and
// found them byte for byte the same; these fingerprints were recorded from the old work(),
// on arm64 (the decoder runs VOLK's neonspiral kernel there).
TEST_CASE("CCDecoder: decoding is unchanged since the deprecated work(in, out, size) was removed")
{
    struct Pin
    {
        int frame;
        uint64_t random_symbols; // pure noise
        uint64_t coded;          // coded bits, noise up to +-160 and one symbol in 7 lost
    };
    const Pin pins[] = {
        {1024, 9399719509068540333ULL, 1809270624834282070ULL},
        {2048, 11859019098497034992ULL, 17891641883411257556ULL},
        {3072, 17532294927862428726ULL, 12539687984905869121ULL},
        {4096, 13026592518857460912ULL, 9214198211198128992ULL},
        {8192, 12021301467964131444ULL, 8312683458005758847ULL},
    };
    for (const Pin &p : pins)
    {
        CAPTURE(p.frame);
        Bytes noise = random_softs(p.frame, 6, 7 + p.frame);
        Bytes bits = random_bits(p.frame * 6 + TAIL, 8 + p.frame);
        Bytes coded = coded_softs(bits, 160, 7, 9 + p.frame);
        CHECK(fnv1a(decode(noise, p.frame, 6)) == p.random_symbols);
        CHECK(fnv1a(decode(coded, p.frame, 6)) == p.coded);
    }
}
