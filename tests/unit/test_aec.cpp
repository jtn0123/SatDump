// Fork: CCSDS 121 (Rice) decoding with libaec, called the way the plugins call it:
//   VIIRS     aec_decode: 15-bit samples, blocks of 8, a reference sample every 128 blocks,
//             MSB first, preprocessed; the output is kept even when decoding stops early
//   OMPS      SZ_BufftoBuffDecompress: 32-bit pixels, 32 per block, 256 per line, NN | MSB,
//             into a 1024 x 3000 byte buffer; used only when it returns AEC_OK
//   GOES LRIT SZ_BufftoBuffDecompress: 8- or 16-bit pixels, 16 per block, one line per packet,
//             K13 | MSB | NN | RAW; used only when it returns AEC_OK
// SatDump decodes with Debian's libaec on Linux and with the bundled copy (src-core/libs/aec,
// libaec 1.0.x) elsewhere (cmake/fork/codecs.cmake).
//
// The vectors below were encoded once with Debian's libaec 1.1.3 encoder (aec_buffer_encode and
// SZ_BufftoBuffCompress) from make_data(). The bundled decoder and Debian's decoder both give
// the data back exactly, and give the same result for every cut-off copy of a vector (the cut
// hashes, taken from the bundled decoder). They differ only on corrupted data: the bundled
// decoder read past its tables and the input on some of it, and Debian's refuses it.
//
// A cut-off GOES line that is not a whole number of blocks is decoded by both into a buffer of
// libaec's own, and the whole line is copied out, the part never decoded included (heap memory,
// different from run to run; libaec 1.1.8 fixes this). The cut hashes cover only the part
// decoded (sz_decoded_bytes).

#include "doctest.h"
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

extern "C"
{
#include <libaec.h>
#include <szlib.h>
}

namespace
{
    namespace hex
    {
        // viirs_smooth: 569 bytes
        const char *const viirs_smooth =
            "87f37f399325489d164473f1a6508f2244aa113e72a6cc9132649a11e7ca894254b993a244f9cc7f1244c950e4479f42"
            "24894f62489513f33a3cc9d2a446871e7c8952a74ba11a544fcca8b4234e8d2a0c79f0a748990624e9919f30a645993a"
            "4488b1e7cc911a64189424467cc895063489d321c7cf0a5529509ec2a119f3129499d2244e8b1f3c6914224887421467"
            "f4a443950a9429d07cf0624c951a1ce9519fca9916542a522441f3c3893a34c8f2a8473f2254c972a8449507cefe44e9"
            "50a0cc911cfce89222cc992a741f9cb952284695168473f222508f1a9449907e70624c8d4254b991cfca9919fc499228"
            "44f9c88f2a64c952e7479f32546911674e8d13e71a0c8953a6478d1e7ce894233f892a744fcca972a948892a2c79f2a6"
            "4e992664a9513f32a3c6953a44c8b1e7c8913a645992a5467cc4991664895322c79f32546991e54a9119f33a54795326"
            "46871f3c88d4a448932a8467cc49d1644e95322c7cf0a7489d121ce9519fc4950674a853a541f3c1853a44887226467f"
            "3274b992a34e9507cf0e54699324ca911cfca992a1c69532841f3cd994214c8f4a5473f0a6508b1264a8d07e6f624c8d"
            "4a449911cfce9d2a4c8993a241f9bf8d3a14c990e2473f42648872a14e9d13e7122ca9d1a641911e7c899224459d2a64"
            "4f9c483322508d063479f1a650912e5509113f319fca8932748831e7c2a132243992a544fcca9742350911a1c79f2a74"
            "a8d1e8469519f32a6499d3214c871e7ce853a54191125467cd0890e44c952a0c7cf3244c951a2ca980";

        // viirs_noise: 495 bytes
        const char *const viirs_noise =
            "fa5041653562cb0d287c30a8864f2f1f6a1d964999c14f4cd6b3cbb9f22fc4f8b13a59a10345807a51b5819bff83ef79"
            "56c2d70e732b1df98330b1038aeaffde6f6433b8508a69da47878594181f3d53c8accc5a2a8d2a7902d4770b84ea9752"
            "ac634d5c02fbbf9d652e92acbdfb889c09783a834aa9406ea00ff077e514a07eb9fa0438af3651abe6ff0bfe03e2585a"
            "e11563b927ced8a7fba417fcb607b8352b0825697d28eb8a9005ffa044fdd9da4bd2d7932d55c5e733f79bb706e40f94"
            "b18f568b1f7346e7ffd30ed7943739365b61894a4062d3febfe5f44acb3c15bb822de22413f9d143cee11aa87acdd80b"
            "c3d25b6ed4c6877d80e0b642c5999dab0534056f6adb2fb7236a87d8a8603c4a4efe62997022f178c9a78c44a8508f0e"
            "df4250839b1ff6f826d64dc5be12f793014664c445ff36b7b7a400c72aba7186fb0ba7ebff13da3f518d09e8711937a3"
            "bfb252c5fc85a1051b7ba3e2129896562673782eed1725e46fb476f415809fddeca47bde6139b14d5b94f849b1c1bd0b"
            "db5c3ce03b44aab84ccab479918000a4b22b9dc963656202c6e478efe978c6d29ffc0e2e8100915e39a202c617351eaf"
            "ef3dcfae5786edcd97014089e4e1579ff25679596fd207216a3ef9d2c4ce686f4a7bfa6e2be2b93c65839481260a15f0"
            "d131a61fdb38c7b8953516ac7a91e0";

        // viirs_constant: 9 bytes
        const char *const viirs_constant =
            "004d20802009a41004";

        // viirs_mixed: 939 bytes
        const char *const viirs_mixed =
            "0004d00b8003fc069a69a69a69bff5ad6b5ad69bff5ad6b5ad69bff5ad6b5ad69bff5ad6b5ad69bff5ad6b5ad69bff5a"
            "d6b5ad69bff5ad6b5ad6a4003ff440000000000000300007fff5000000000000000000060000fffea000000000000000"
            "0000c0001fffd4000000000000000000180003fffa8000000000000000000300007fff5000000000000000000060000f"
            "ffea0000000000000000000c0001fffd4000000000000000000180003ff058040060080000100400778080f002001002"
            "fffe0900100200325aaca4795088c230de8080fffffc14004002002000000004285ca4ad2b9470007f9e000000000000"
            "0640003fd0068686868686869bff5ad6b5ad69bff5ad6b5ad69bff5ad6b5ad69bff5ad6b5ad69bff5ad6b5ad69bff5ad"
            "6b5ad69bff5ad6b5ad6ac01ff6880000000000000000180003fffa8000000000000000000300007fff50000000000000"
            "00000060000fffea0000000000000000000c0001fffd4000000000000000000180003fffa80000000000000000003000"
            "07fff5000000000000000000060000fffea0000000000000000000c0001ff82c000000080000080100424497e8080fff"
            "ffc140040000000000020002630c64596549df8080c010000000000007fff828000a557a410dca5c001fe88000000000"
            "0001a0003fe801a0d068341a0d069bff5ad6b5ad69bff5ad6b5ad69bff5ad6b5ad69bff5ad6b5ad69bff5ad6b5ad69bf"
            "f5ad6b5ad69bff5ad6b5ad6ac0003fcd100000000000000000300007fff5000000000000000000060000fffea0000000"
            "000000000000c0001fffd4000000000000000000180003fffa8000000000000000000300007fff500000000000000000"
            "0060000fffea0000000000000000000c0001fffd4000000000000000000180003ff0500000201000801000008ad69a2d"
            "f8080c00c01000400800ffff82c0008dd521236ac889d5344dc0407c00400bfff82800800400000095d4a138003fd100"
            "0000000000036007fc800d0340d0340d0340d37feb5ad6b5ad37feb5ad6b5ad37feb5ad6b5ad37feb5ad6b5ad37feb5a"
            "d6b5ad37feb5ad6b5ad37feb5ad6b5ad6000ff5a20000000000000000000c0001fffd4000000000000000000180003ff"
            "fa8000000000000000000300007fff5000000000000000000060000fffea0000000000000000000c0001fffd40000000"
            "00000000000180003fffa8000000000000000000300007fff5000000000000000000060000ffc1400001003008004010"
            "0325e72b19673b8080f800001ffff048008008000000c34b443fc0";

        // viirs_extremes: 388 bytes
        const char *const viirs_extremes =
            "f0001fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
            "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
            "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
            "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
            "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
            "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
            "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
            "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
            "fffffff0";

        // viirs_lowentropy: 252 bytes
        const char *const viirs_lowentropy =
            "107d5881a4b6ab9252aa28a92dd2904ab63344b5c6307a631b0ce44767182854e4acd2644ae39262915554891b6d2163"
            "cbba42de5a0923535924898ea24c748c51729ea8b25b54251dab44a52aaa125bb4c385988e1c4222651c09255274a284"
            "e394ac5cd154a71226e259223b891921b55492925649896ea924526e564593c54b899840d860e25c286546e74484daa8"
            "4578d2b2b2929213133740b8e5ed29bcd41bbd66ee22de3484f12e94874a3341a24a64b1b5a8894a53524537518b4664"
            "90b729222645e5245294d24a4e7a029273ac2a5519208cb4911c2d2513293445524ca51137589225374b2269e8a3d30c"
            "6c6712e14642c4eb40a66448";

        // omps_smooth: 831 bytes
        const char *const omps_smooth =
            "2ff9dcdcecee7662628344c4834113120d06898a08d04c483413131141a262834131120d04c4c5068898a0d04c483441"
            "a2628344c4fff0000ff0000ff0000ff00ff0000ff0000ff00ff0000ff0000ff0000ff00ff00e01fe0001fe01fe0001fe"
            "0001fe01fe0001fe0001fe0001fe01fe0001fe0001ffc03fc0003fc0003fc03fc0003fc0003fc0003fc03fc0003fc000"
            "3fc03fc0003ff80007f80007f807f80007f80007f807f80007f80007f807f80007f80007f80007ff00ff0000ff0000ff"
            "00ff0000ff0000ff00ff0000ff0000ff0000ff00ff0000ffe0001fe01fe0001fe0001fe01fe0001fe0001fe0001fe01f"
            "e0001fe0001fe01ffc0003fc0003fc03fc0003fc0003fc0003fc03fc0003fc0003fc03fc0003fc03807f80007f807f80"
            "007f80007f807f80007f80007f807f80007f80007f80007ffff0000ff0000ff0000ff00ff0000ff0000ff00ff0000ff0"
            "000ff0000ff00ff00e01fe0001fe01fe0001fe0001fe01fe0001fe0001fe0001fe01fe0001fe0001ffc03fc0003fc000"
            "3fc03fc0003fc0003fc0003fc03fc0003fc0003fc03fc0003ff80007f80007f807f80007f80007f807f80007f80007f8"
            "07f80007f80007f80007ff00ff0000ff0000ff00ff0000ff0000ff00ff0000ff0000ff0000ff00ff0000ffe0001fe01f"
            "e0001fe0001fe01fe0001fe0001fe0001fe01fe0001fe0001fe01ffc0003fc0003fc03fc0003fc0003fc0003fc03fc00"
            "03fc0003fc03fc0003fc03807f80007f807f80007f80007f807f80007f80007f807f80007f80007f80007ff9b4c4cf33"
            "04eb44846de49a54848ed3a4ab24cfd284ca9504ae7404ab44cf32ee99688891be8b4690a1dc6a957689fc4c9b52989d"
            "c87e8d7891ec5c9d5ea08dc1d1285411bb0d92ae90bf49932ad113b94ed32e933d0bd22c13123890e891143bf9a255c2"
            "3246ca6552125731f2160217b15a957a72470a2d123247719a45aa8217d848a94a42e7394ebe44f82c46ad4e4ae144a1"
            "4a42ef3448b6444edd48a44a50fd474996895f052955c909dc27f3c9891e06291649899b895489491d4709574a1fe4a1"
            "42b1194b95027d1133d4c92ac5312b7516951133b8e522e12beca92aa53ba6721f245d277918235924256ea452232777"
            "9c235da27e94a5542529729e255d00";

        // omps_mixed: 492 bytes
        const char *const omps_mixed =
            "0001bfe0003fc0007f8000ff00000000f0000000f0000000f0000000a0003fc0007f8000ff0001ffe0000001e0000001"
            "e0000001e000000140007ff00010001fffffe0000000001fe00000000000000000017fc00040007fffff00000000ff00"
            "00000000000000000000000037fc0007f8000ff0001fe00000001e0000001e0000001e000000140007f8000ff0001fe0"
            "003ffc0000003c0000003c0000003c0000002807fe00020003ffffc0000000003fc0000000000000000002ff80008000"
            "fffffe00000001fe000000000000000000000002207f07fc60ffc1fa01fc0007f8000ff0001fe0000001e0000001e000"
            "0001e000000140007f8000ff0001fe0003ffc0000003c0000003c0000003c00000028000ffe00020003fffff80000000"
            "003f80000000000000000002ff80008000fffffe00000001fc00000000000000000000002ffff0a0000000003fffffff"
            "e000000003903fff01ffffd6b5ad6b5ad6b5ad6b90d6b5ad6b5ad6b5ad6b5b80fffff01ffd6b58a6b5ad6b5ad6b5ad6b"
            "5ad6b54c6b5ad6b5aa000ff0001fe0003fc0007f8800000028000000280000002800000050001fe0003fc0007f8000ff"
            "5000000050000000500000005000000052bb68010d56abee5a54d290d52cf0020becf6da5644314528c00000000003ff"
            "fffffe000000000000000020";

        // goes8_mixed: 402 bytes
        const char *const goes8_mixed =
            "04d1dfffc073ad6b5ad6b5ad6b5ad737fffc35ad6b5ad6b5ad6b5ad6e03fffad6b14d6b5ad6b5ad6b5bf80ffeb5ad6b5"
            "aa635ad6b5ad50007f8000ff1000000050000000a0003fc0007fa80000002800000050001fe0003fd400000014000000"
            "28000ff0001fea0000000a0000000a576d0021ad2a6a5aafb94a4354b3c0082fa4430a7b6d2ba2948000007fffa00000"
            "00000001dfffc073ad6b5ad6b5ad6b5ad737fffc35ad6b5ad6b5ad6b5ad6e03fffad6b14d6b5ad6b5ad6b5bf80ffeb5a"
            "d6b5aa635ad6b5ad50007f8000ff1000000050000000a0003fc0007fa80000002800000050001fe0003fd40000001400"
            "000028000ff0001fea0000000a00000005c3524497a0020bf6cd811122cb29f0020d2420a112470dca6000001fffe800"
            "000000000077fff01ceb5ad6b5ad6b5ad6b5cdffff0d6b5ad6b5ad6b5ad6b5b80fffeb5ac535ad6b5ad6b5ad6fe03ffa"
            "d6b5ad6a98d6b5ad6b54001fe0003fc40000001400000028000ff0001fea0000000a000000140007f8000ff500000005"
            "0000000a0003fc0007fa8000000280000000";

        // goes8_noise: 311 bytes
        const char *const goes8_noise =
            "f04d646b25855b9e201ae3f7c246e4b89dda67e51aca7cfe04b3e4c62a0e53e1f3efc847ad7f795933d48232d40cfa41"
            "3dfa944d834038c2db2713f3ef2b64907dffadd52dc4322991a106f576b4869e048be4d1d33d9c638645fdc8a2ed21ea"
            "ab3f78f07ed9eb104468001ca318737c1f488fbeac1f63e3db03af26e9c96885612077e39ba59a3ea0a9561eeae60773"
            "ace5445ce5de2541e1d041390ad4147af3ea8117b86f1fcb3108b2b476fe50c3dac7322a71a149fe2776a2270d040f9e"
            "ec079260fefe9d5a43b236a6c20e954fa54efe830ddb1de711c817bed808813771bf8fbb093ba9feeec8513fc4fd9b28"
            "9e562ba4cc37c29b306cd3524c15896749defcba70e1fa922f9617c6a5209410d01f8b92e9b8deb9b8e7ec83f8c70366"
            "6d2b207189e5fb20e4c080a09ffa205686a7dc90800000";

        // goes16_smooth: 655 bytes
        const char *const goes16_smooth =
            "87f9bf9f3f3264a913a2c88d32847912255089f3f3e54d992264c9342544a12a5cc9d1227cfcf98fe248992a1c8a1124"
            "4a7b1244a89f9f3e747993a5488d0e44a953a5d08d2a27e7cf95168469d1a541853a44c831274c8cf9f3e14c8b327489"
            "1166488d320c4a12233e7e7912a0c6913a643852a94a84f61508cf9f9e252933a4489d16348a112443a10a33f3e7a522"
            "1ca854a14e831264a8d0e74a8cfcf9e54c8b2a15291221c49d1a647954239f3e7912a64b954224a7f2274a85066488e7"
            "cfce744911664c953a5ca914234a8b4239f3f391128478d4a24c8312646a12a5cc8e7e7ce54c8cfe24c9142447953264"
            "a973a3cf9f3992a3488b3a7468d0644a9d323c68f3e7e6744a119fc4953a54b954a44495163cf9f9953274c933254a95"
            "1e34a9d226458f3f3e64489d322cc952a24c8b3244a99163cfcf9992a34c8f2a5489d2a3ca99323438f9f3e6446a5224"
            "49954224e8b2274a99163e7cfe14e913a2439d2a24a833a5429d2a0f9f3f830a748910e44c993a5cc951a74a83e7e7e1"
            "ca8d32649952254c950e34a99420f9f9f9b32842991e94a8532845893254683f3e7dec4991a948932274e952644c9d12"
            "0fcf9f7f1a74299321c4a132443950a74e89f3f3e245953a34c832244c91222ce953227cfcf8906644a11a0c68d32848"
            "972a84889f9f3e33f951264e910615099121cc952a27e7cf952e846a122343953a5468f4234a8cf9f3e54c933a642990"
            "e7429d2a0c8892a33e7e7a1121c8992a541992264a8d1654c8cf9f9e6427b1264c993674a952a645894233f3e7992a4c"
            "e89425447f2244a911e9448cfcf9e646832a34e8d423cc952a243992a39f3e79d1a441951294a9b4a348991e64a8e7cf"
            "ce64c84f634a9d223c8a12a4467f19892004927fd469364652000000000000";
    } // namespace hex

    enum Kind
    {
        VIIRS,
        OMPS,
        GOES8,
        GOES16
    };

    struct Vector
    {
        const char *name;
        Kind kind;
        int pattern;       // see sample()
        size_t samples;    // how many samples make_data() makes
        const char *hex;   // the encoded data
        uint64_t cut_hash; // cut_hash() of the bundled decoder (0 for OMPS, not checked)
    };

    const Vector vectors[] = {
        {"viirs_smooth", VIIRS, 0, 512, hex::viirs_smooth, 0xa63bdf0524bd24a1ull},
        {"viirs_noise", VIIRS, 1, 256, hex::viirs_noise, 0xd013152ad2b99bf3ull},
        {"viirs_constant", VIIRS, 2, 2048, hex::viirs_constant, 0x675d83b5646716cfull},
        {"viirs_mixed", VIIRS, 3, 1001, hex::viirs_mixed, 0x0b5519e72383a165ull},
        {"viirs_extremes", VIIRS, 4, 200, hex::viirs_extremes, 0x78761d55ceb566f1ull},
        {"viirs_lowentropy", VIIRS, 5, 600, hex::viirs_lowentropy, 0xfee359bf9e95963full},
        {"omps_smooth", OMPS, 0, 256, hex::omps_smooth, 0},
        {"omps_mixed", OMPS, 3, 320, hex::omps_mixed, 0},
        {"goes8_mixed", GOES8, 3, 700, hex::goes8_mixed, 0x6c6cd2f0e999550full},
        {"goes8_noise", GOES8, 1, 300, hex::goes8_noise, 0x867cd5c7252945b1ull},
        {"goes16_smooth", GOES16, 0, 600, hex::goes16_smooth, 0xd49ef62b2f6d3f6full},
    };

    int bits_of(Kind kind) { return kind == VIIRS ? 15 : kind == OMPS ? 32 : kind == GOES8 ? 8 : 16; }
    int bytes_of(Kind kind) { return kind == VIIRS ? 2 : kind == OMPS ? 4 : kind == GOES8 ? 1 : 2; }

    uint32_t lcg(uint32_t &s)
    {
        s = s * 1664525u + 1013904223u;
        return s >> 8;
    }

    uint32_t sample(int pattern, size_t i, int bits, uint32_t &s)
    {
        const uint32_t max = bits == 32 ? 0xFFFFFFFFu : ((1u << bits) - 1);
        uint32_t v;
        switch (pattern)
        {
        case 0: // smooth
            v = (uint32_t)(max / 2 + (int32_t)((i * 37) % 200) - 100 + (int32_t)(lcg(s) % 5));
            break;
        case 1: // noise
        {
            uint32_t a = lcg(s); // in this order
            uint32_t b = lcg(s);
            v = a ^ (b << 16);
            break;
        }
        case 2: // constant
            v = 1234;
            break;
        case 3: // constant runs, ramps, steps and spikes
            if ((i / 64) % 4 == 0)
                v = 77;
            else if ((i / 64) % 4 == 1)
                v = (uint32_t)(i * 13);
            else if ((i / 64) % 4 == 2)
                v = (i % 16) < 8 ? 10 : max - 10;
            else
                v = (i % 29) == 0 ? max : (uint32_t)(500 + (lcg(s) % 3));
            break;
        case 4: // extremes
            v = (i & 1) ? max : 0;
            break;
        default: // low entropy
            v = 1000 + (lcg(s) % 4);
            break;
        }
        return v & max;
    }

    // The data a vector was encoded from, big-endian samples
    std::vector<uint8_t> make_data(const Vector &v)
    {
        const int bytes = bytes_of(v.kind), bits = bits_of(v.kind);
        std::vector<uint8_t> d(v.samples * bytes);
        uint32_t s = 12345u + (uint32_t)v.pattern;
        for (size_t i = 0; i < v.samples; i++)
        {
            const uint32_t x = sample(v.pattern, i, bits, s);
            for (int b = 0; b < bytes; b++)
                d[i * bytes + b] = (uint8_t)(x >> (8 * (bytes - 1 - b)));
        }
        return d;
    }

    std::vector<uint8_t> from_hex(const char *text)
    {
        std::vector<uint8_t> out;
        const size_t n = strlen(text);
        for (size_t i = 0; i + 1 < n; i += 2)
            out.push_back((uint8_t)std::stoi(std::string(text + i, 2), nullptr, 16));
        return out;
    }

    SZ_com_t sz_params(const Vector &v)
    {
        SZ_com_t p;
        memset(&p, 0, sizeof(p));
        if (v.kind == OMPS)
        {
            p.pixels_per_scanline = 256;
            p.bits_per_pixel = 32;
            p.pixels_per_block = 32;
            p.options_mask = SZ_NN_OPTION_MASK | SZ_MSB_OPTION_MASK;
        }
        else
        {
            p.bits_per_pixel = bits_of(v.kind);
            p.pixels_per_block = 16;
            p.pixels_per_scanline = (int)v.samples;
            p.options_mask = SZ_ALLOW_K13_OPTION_MASK | SZ_MSB_OPTION_MASK | SZ_NN_OPTION_MASK | SZ_RAW_OPTION_MASK;
        }
        return p;
    }

    // The output buffer each plugin decodes into
    size_t out_size(const Vector &v) { return v.kind == OMPS ? 1024 * 3000 : v.samples * bytes_of(v.kind); }

    struct Decoded
    {
        int ret = 0;
        size_t produced = 0;
        std::vector<uint8_t> out; // starts filled with 0xA5
        size_t defined = 0;       // how much of out the decoder decoded or left alone
    };

    // How many bytes of a GOES line the SZ wrapper really decoded. On a line that is not a whole
    // number of blocks, it decodes into a buffer of its own and copies the whole line out, so
    // cut-off data leaves heap memory in the rest (libaec 1.0.x and 1.1.3; 1.1.8 copies only the
    // part decoded). The same data through aec_decode, with the settings the wrapper uses, gives
    // the number of bytes decoded.
    size_t sz_decoded_bytes(const Vector &v, const std::vector<uint8_t> &in, size_t line)
    {
        if (v.samples % 16 == 0)
            return line;
        const size_t padded = (v.samples + 15) / 16 * 16 * bytes_of(v.kind);
        std::vector<uint8_t> buf(padded);
        aec_stream s;
        memset(&s, 0, sizeof(s));
        s.bits_per_sample = bits_of(v.kind);
        s.block_size = 16;
        s.rsi = (v.samples + 15) / 16;
        s.flags = AEC_DATA_MSB | AEC_DATA_PREPROCESS;
        s.next_in = in.data();
        s.avail_in = in.size();
        s.next_out = buf.data();
        s.avail_out = buf.size();
        size_t done = 0;
        if (aec_decode_init(&s) == AEC_OK)
        {
            aec_decode(&s, AEC_FLUSH);
            done = s.total_out;
            aec_decode_end(&s);
        }
        return done < line ? done : line;
    }

    Decoded decode(const Vector &v, const uint8_t *in, size_t len)
    {
        Decoded d;
        d.out.assign(out_size(v), 0xA5);
        // A copy of exactly len bytes, so a sanitizer build sees any read past the end
        std::vector<uint8_t> copy(in, in + len);
        if (v.kind == VIIRS)
        {
            aec_stream s;
            memset(&s, 0, sizeof(s));
            s.bits_per_sample = 15;
            s.block_size = 8;
            s.rsi = 128;
            s.flags = AEC_DATA_MSB | AEC_DATA_PREPROCESS;
            s.next_in = copy.data();
            s.avail_in = len;
            s.next_out = d.out.data();
            s.avail_out = d.out.size();
            aec_decode_init(&s);
            d.ret = aec_decode(&s, AEC_FLUSH);
            aec_decode_end(&s);
            d.produced = s.total_out;
        }
        else
        {
            SZ_com_t p = sz_params(v);
            d.produced = d.out.size();
            d.ret = SZ_BufftoBuffDecompress(d.out.data(), &d.produced, copy.data(), len, &p);
        }
        d.defined = d.out.size();
        if (v.kind != VIIRS && v.kind != OMPS && d.ret == AEC_OK)
            d.defined = sz_decoded_bytes(v, copy, d.out.size());
        return d;
    }

    void fnv_add(uint64_t &h, const uint8_t *d, size_t n)
    {
        for (size_t i = 0; i < n; i++)
            h = (h ^ d[i]) * 1099511628211ull;
    }

    void fnv_add_le(uint64_t &h, uint64_t v, int bytes)
    {
        for (int i = 0; i < bytes; i++)
        {
            const uint8_t b = (uint8_t)(v >> (8 * i));
            fnv_add(h, &b, 1);
        }
    }

    // One hash over every cut-off copy of a vector (0 to all but the last byte): for each, the
    // return code (4 bytes), the bytes produced (8 bytes) and the output buffer (all of it, or
    // for GOES the part decoded)
    uint64_t cut_hash(const Vector &v, const std::vector<uint8_t> &in)
    {
        uint64_t h = 1469598103934665603ull;
        for (size_t cut = 0; cut < in.size(); cut++)
        {
            Decoded d = decode(v, in.data(), cut);
            fnv_add_le(h, (uint32_t)(int32_t)d.ret, 4);
            fnv_add_le(h, d.produced, 8);
            fnv_add(h, d.out.data(), d.defined);
        }
        return h;
    }

    // A copy, found by name
    Vector find_vector(const char *name)
    {
        for (const Vector &v : vectors)
            if (strcmp(name, v.name) == 0)
                return v;
        throw std::runtime_error(std::string("no vector ") + name);
    }
} // namespace

TEST_CASE("libaec: VIIRS, OMPS and GOES packets decode to exactly the data they were made from")
{
    for (const Vector &v : vectors)
    {
        CAPTURE(v.name);
        const std::vector<uint8_t> in = from_hex(v.hex);
        const std::vector<uint8_t> expected = make_data(v);
        Decoded d = decode(v, in.data(), in.size());
        CHECK(d.ret == AEC_OK);
        REQUIRE(d.produced == expected.size());
        CHECK(memcmp(d.out.data(), expected.data(), expected.size()) == 0);
    }
}

TEST_CASE("libaec: cut-off VIIRS and GOES packets decode as with the bundled decoder")
{
    // VIIRS keeps whatever was decoded before the data ran out, so this must not change
    for (const Vector &v : vectors)
    {
        if (v.kind == OMPS)
            continue;
        CAPTURE(v.name);
        CHECK(cut_hash(v, from_hex(v.hex)) == v.cut_hash);
    }
}

// This checks the system libaec that Linux builds use. The bundled copy that Windows, macOS and
// Android builds still use (src-core/libs/aec, 1.0.x, left as it is) accepts some of these runs
// and returns garbage or reads past its table, so this case fails there until that copy is
// brought up to libaec 1.1.x.
TEST_CASE("libaec: a run of zero bits in a packet is refused (the bundled decoder read past its table or the input)")
{
    struct Case
    {
        const char *vector;
        size_t offset; // where 24 bytes are set to 0
    };
    const Case cases[] = {{"omps_smooth", 17}, {"omps_smooth", 34}, {"omps_smooth", 51}, {"viirs_mixed", 901}};
    for (const Case &c : cases)
    {
        CAPTURE(c.vector);
        CAPTURE(c.offset);
        const Vector v = find_vector(c.vector);
        std::vector<uint8_t> in = from_hex(v.hex);
        REQUIRE(c.offset + 24 <= in.size());
        memset(in.data() + c.offset, 0, 24);
        CHECK(decode(v, in.data(), in.size()).ret == AEC_DATA_ERROR);
    }
}
