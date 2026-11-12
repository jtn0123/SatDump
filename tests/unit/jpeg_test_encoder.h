// Fork: makes JPEG streams for the tests with one of the bundled libjpeg copies. Include
// libs/jpeg/jpeglib.h (8-bit) or libs/jpeg12/jpeglib12.h (12-bit) in an extern "C" block first;
// the encoder then uses that copy. Every encoder setting SatDump's own saving code does not use
// (12-bit, progressive, lossless, restart markers, RGB and CMYK) is reachable from here.
#pragma once

// jconfig.h and jconfig12.h define const as nothing (an old configure setting for C compilers
// without it), which would also take it out of the C++ below
#undef const

#include <csetjmp>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

// The two copies' headers declare structs of the same names with different layouts, so the
// functions below differ between a test file using the 8-bit copy and one using the 12-bit one.
// Each gets its own namespace: the linker would otherwise keep one of the two and use it for both.
#if BITS_IN_JSAMPLE == 12
#define JPEG_TEST_COPY copy12
#else
#define JPEG_TEST_COPY copy8
#endif

namespace jpeg_test
{
inline namespace JPEG_TEST_COPY
{
    struct EncodeSettings
    {
        int width = 33;
        int height = 19;
        int components = 1;     // 1 grey, 3 colour (stored as YCbCr unless rgb), 4 CMYK
        bool rgb = false;       // 3 components stored as RGB (Adobe marker, no colour transform)
        int h_samp = 2;         // sampling of the first of 3 components (2x2: 4:2:0)
        int v_samp = 2;
        int quality = 75;
        bool progressive = false;
        bool optimize = false;   // Huffman tables made for the picture (always, in the 12-bit copy)
        int restart_interval = 0; // in MCUs; 0 for none
        int lossless_predictor = 0; // 1 to 7: lossless (SOF3) with this predictor
        int point_transform = 0;    // lossless only
    };

    namespace encoder_detail
    {
        // Everything the libjpeg calls use, in one struct with no destructor: libjpeg reports an
        // error by longjmp-ing out of its work, which may not skip a C++ destructor
        struct State
        {
            jpeg_error_mgr pub; // first, so the error manager pointer is the state's address
            jmp_buf jump;
            jpeg_compress_struct cinfo;
            jpeg_destination_mgr dest;
            JOCTET *out;
            size_t out_size;
            size_t out_capacity;
            JSAMPLE *row;
        };

        inline State *state_of(j_common_ptr cinfo) { return (State *)cinfo->err; }

        inline void error_exit(j_common_ptr cinfo) { longjmp(state_of(cinfo)->jump, 1); }

        inline void init_destination(j_compress_ptr cinfo)
        {
            State *st = state_of((j_common_ptr)cinfo);
            st->out_capacity = 4096;
            st->out = (JOCTET *)malloc(st->out_capacity);
            if (st->out == nullptr)
                error_exit((j_common_ptr)cinfo);
            st->dest.next_output_byte = st->out;
            st->dest.free_in_buffer = st->out_capacity;
        }

        // The return type is boolean in the 12-bit copy and jboolean in the 8-bit one
        inline decltype((*((jpeg_destination_mgr *)nullptr)->empty_output_buffer)(nullptr)) empty_output_buffer(j_compress_ptr cinfo)
        {
            State *st = state_of((j_common_ptr)cinfo);
            size_t used = st->out_capacity;
            JOCTET *bigger = (JOCTET *)realloc(st->out, st->out_capacity * 2);
            if (bigger == nullptr)
                error_exit((j_common_ptr)cinfo);
            st->out = bigger;
            st->out_capacity *= 2;
            st->dest.next_output_byte = st->out + used;
            st->dest.free_in_buffer = st->out_capacity - used;
            return TRUE;
        }

        inline void term_destination(j_compress_ptr cinfo)
        {
            State *st = state_of((j_common_ptr)cinfo);
            st->out_size = st->out_capacity - st->dest.free_in_buffer;
        }

        // Fills st->out (malloc'ed) and returns true, or returns false with nothing to free
        inline bool encode_into(State *st, const EncodeSettings *s, const int *samples)
        {
            st->out = nullptr;
            st->out_size = 0;
            st->out_capacity = 0;
            st->row = nullptr;
            st->cinfo.err = jpeg_std_error(&st->pub);
            st->pub.error_exit = error_exit;
            if (setjmp(st->jump))
            {
                jpeg_destroy_compress(&st->cinfo);
                free(st->row);
                free(st->out);
                st->row = nullptr;
                st->out = nullptr;
                return false;
            }
            jpeg_create_compress(&st->cinfo);
            st->dest.init_destination = init_destination;
            st->dest.empty_output_buffer = empty_output_buffer;
            st->dest.term_destination = term_destination;
            st->cinfo.dest = &st->dest;

            st->cinfo.image_width = s->width;
            st->cinfo.image_height = s->height;
            st->cinfo.input_components = s->components;
            st->cinfo.in_color_space = s->components == 1 ? JCS_GRAYSCALE : (s->components == 3 ? JCS_RGB : JCS_CMYK);
            jpeg_set_defaults(&st->cinfo);
            if (s->components == 3 && s->rgb)
                jpeg_set_colorspace(&st->cinfo, JCS_RGB);
            if (s->lossless_predictor > 0)
                jpeg_simple_lossless(&st->cinfo, s->lossless_predictor, s->point_transform);
            else
            {
                jpeg_set_quality(&st->cinfo, s->quality, FALSE);
                if (s->progressive)
                    jpeg_simple_progression(&st->cinfo);
            }
            if (s->components == 3 && !s->rgb)
            {
                st->cinfo.comp_info[0].h_samp_factor = s->h_samp;
                st->cinfo.comp_info[0].v_samp_factor = s->v_samp;
            }
            if (s->optimize) // the 12-bit copy always makes its tables: the standard ones are for 8 bits
                st->cinfo.optimize_coding = TRUE;
            st->cinfo.restart_interval = s->restart_interval;
            jpeg_start_compress(&st->cinfo, TRUE);

            const size_t row_samples = (size_t)s->width * s->components;
            st->row = (JSAMPLE *)malloc(sizeof(JSAMPLE) * row_samples);
            if (st->row == nullptr)
                error_exit((j_common_ptr)&st->cinfo);
            while (st->cinfo.next_scanline < st->cinfo.image_height)
            {
                const int *src = samples + (size_t)st->cinfo.next_scanline * row_samples;
                for (size_t i = 0; i < row_samples; i++)
                    st->row[i] = (JSAMPLE)src[i];
                JSAMPROW rows[1] = {st->row};
                jpeg_write_scanlines(&st->cinfo, rows, 1);
            }
            jpeg_finish_compress(&st->cinfo);
            jpeg_destroy_compress(&st->cinfo);
            free(st->row);
            st->row = nullptr;
            return true;
        }
    } // namespace encoder_detail

    // The stream, or nothing when libjpeg refused the settings. samples: width x height pixels,
    // components interleaved, each from 0 to MAXJSAMPLE of the copy used
    inline std::vector<uint8_t> encode(const EncodeSettings &s, const std::vector<int> &samples)
    {
        encoder_detail::State st;
        if (samples.size() < (size_t)s.width * s.height * s.components || !encoder_detail::encode_into(&st, &s, samples.data()))
            return std::vector<uint8_t>();
        std::vector<uint8_t> out(st.out, st.out + st.out_size);
        free(st.out);
        return out;
    }

    struct NamedSettings
    {
        const char *name;
        EncodeSettings s;
    };

    inline EncodeSettings settings(int width, int height, int quality)
    {
        EncodeSettings s;
        s.width = width;
        s.height = height;
        s.quality = quality;
        return s;
    }

    // One-component streams with every encoder setting: what decompress_jpeg and
    // decompress_jpeg12 are for
    inline std::vector<NamedSettings> grey_settings()
    {
        std::vector<NamedSettings> all;
        all.push_back({"grey 33x19 q75", settings(33, 19, 75)});
        all.push_back({"grey 1x1 q75", settings(1, 1, 75)});
        all.push_back({"grey 7x5 q90", settings(7, 5, 90)});
        EncodeSettings opt = settings(64, 48, 95);
        opt.optimize = true;
        all.push_back({"grey 64x48 q95 optimized", opt});
        EncodeSettings prog = settings(33, 19, 75);
        prog.progressive = true;
        all.push_back({"grey 33x19 progressive", prog});
        EncodeSettings prog2 = settings(64, 48, 50);
        prog2.progressive = true;
        all.push_back({"grey 64x48 q50 progressive", prog2});
        EncodeSettings rst = settings(33, 19, 75);
        rst.restart_interval = 1;
        all.push_back({"grey 33x19 restart every MCU", rst});
        static const char *const lossless_names[] = {"", "grey 33x19 lossless p1", "grey 33x19 lossless p2", "grey 33x19 lossless p3",
                                                     "grey 33x19 lossless p4", "grey 33x19 lossless p5", "grey 33x19 lossless p6",
                                                     "grey 33x19 lossless p7"};
        for (int p = 1; p <= 7; p++)
        {
            EncodeSettings ll = settings(33, 19, 75);
            ll.lossless_predictor = p;
            all.push_back({lossless_names[p], ll});
        }
        EncodeSettings llpt = settings(33, 19, 75);
        llpt.lossless_predictor = 1;
        llpt.point_transform = 2;
        all.push_back({"grey 33x19 lossless p1 pt2", llpt});
        EncodeSettings llrst = settings(33, 19, 75);
        llrst.lossless_predictor = 6;
        llrst.restart_interval = 33; // one row
        all.push_back({"grey 33x19 lossless p6 restart every row", llrst});
        return all;
    }

    inline EncodeSettings grey_setting(const char *name)
    {
        for (auto &n : grey_settings())
            if (std::string(n.name) == name)
                return n.s;
        return EncodeSettings();
    }

    // Streams with three or four components
    inline std::vector<NamedSettings> colour_settings()
    {
        std::vector<NamedSettings> all;
        EncodeSettings c = settings(33, 19, 75);
        c.components = 3;
        all.push_back({"ycc420 33x19", c});
        c.h_samp = c.v_samp = 1;
        all.push_back({"ycc444 33x19", c});
        EncodeSettings rgb = settings(33, 19, 75);
        rgb.components = 3;
        rgb.rgb = true;
        all.push_back({"rgb 33x19", rgb});
        EncodeSettings cmyk = settings(33, 19, 75);
        cmyk.components = 4;
        all.push_back({"cmyk 33x19", cmyk});
        return all;
    }
} // namespace JPEG_TEST_COPY
} // namespace jpeg_test
