#include "jpeg_utils.h"
#include "logger.h"
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <exception>
#include <stdexcept>
extern "C"
{
#include "libs/jpeg/jpeglib.h"
}
// Fork: libjpeg's jconfig.h defines const as nothing (an old setting for C compilers without
// it), which also took every const out of the C++ below; libjpeg's headers are read by now
#undef const
// #include "libs/openjp2/openjpeg.h"

namespace satdump
{
    namespace image
    {
        namespace
        {
            // Fork: libjpeg reports an error by calling error_exit, which must not return. With
            // ignore_errors it used to return, and libjpeg carried on with data it had just found
            // to be bad, which it is not written to do (it wrote past its buffers on damaged
            // progressive streams). Now error_exit always longjmps back, keeping libjpeg's message.
            // Everything libjpeg and the jump use is in this struct, which has no destructor and
            // is not a local of the functions that call setjmp, so the longjmp skips no C++
            // destructor and no value is lost to it.
            struct JpegDecode
            {
                jpeg_error_mgr pub; // first: libjpeg passes &pub to error_exit, which is this struct
                jmp_buf jump;
                jpeg_decompress_struct cinfo;
                char message[JMSG_LENGTH_MAX];
                size_t rows_done;
            };

            void decode_error_exit(j_common_ptr cinfo)
            {
                JpegDecode *d = (JpegDecode *)cinfo->err;
                cinfo->err->format_message(cinfo, d->message);
                longjmp(d->jump, 1);
            }

            // Sets up libjpeg and reads the headers; false (with d->message) on an error or when
            // there is no picture. d must be zeroed: jpeg_destroy_decompress is then safe after
            // any error.
            bool read_headers(JpegDecode *d, uint8_t *data, int length)
            {
                if (setjmp(d->jump))
                    return false;
                d->cinfo.err = jpeg_std_error(&d->pub);
                d->pub.error_exit = decode_error_exit; // warnings are still printed by libjpeg, as before
                jpeg_create_decompress(&d->cinfo);
                jpeg_mem__src(&d->cinfo, data, (unsigned long)length);
                if (jpeg_read_header(&d->cinfo, FALSE) != JPEG_HEADER_OK)
                {
                    snprintf(d->message, sizeof(d->message), "no picture in the stream");
                    return false;
                }
                return true;
            }

            // Decodes the picture into rows (width bytes each, height rows); false (with
            // d->message) on an error, after d->rows_done rows
            bool decode_rows(JpegDecode *d, uint8_t *rows, size_t width, size_t height)
            {
                d->rows_done = 0;
                if (setjmp(d->jump))
                    return false;
                // Fork: one sample per pixel, as the buffer holds. The default output of a stream
                // with three components is RGB, three samples per pixel, which libjpeg wrote past
                // the end of the buffer; grey output is the luminance of a YCbCr stream, and
                // libjpeg refuses it for RGB and CMYK streams.
                d->cinfo.out_color_space = JCS_GRAYSCALE;
                jpeg_start_decompress(&d->cinfo);
                if (d->cinfo.output_components != 1 || d->cinfo.output_width != width || d->cinfo.output_height != height)
                {
                    snprintf(d->message, sizeof(d->message), "unexpected output from libjpeg");
                    return false;
                }
                while (d->rows_done < height)
                {
                    JSAMPROW row = rows + d->rows_done * width;
                    if (jpeg_read_scanlines(&d->cinfo, &row, 1) != 1)
                    {
                        snprintf(d->message, sizeof(d->message), "libjpeg gave no row");
                        return false;
                    }
                    d->rows_done++;
                }
                // jpeg_finish_decompress is not called: it would read the rest of the stream
                // after the last row, where an error would lose the picture
                return true;
            }

            // Fork: a stream given no picture. With ignore_errors the caller (MSG and Elektro-L
            // xRIT) keeps whatever it gets, so this is a warning with libjpeg's reason (it was
            // not logged at all); a strict caller decides itself (GK-2A then tries JPEG 2000),
            // so for it this is a debug line
            void refused(bool ignore_errors, const char *reason)
            {
                if (ignore_errors)
                    logger->warn("JPEG not decoded, no picture: %s", reason);
                else
                    logger->debug("JPEG refused: %s", reason);
            }
        } // namespace

        Image decompress_jpeg(uint8_t *data, int length, bool ignore_errors, size_t max_pixels)
        {
            // Fork: a length below 0 (from a caller's size calculation gone wrong) was given to
            // libjpeg as a huge unsigned one, and it read past the end of the data
            if (data == nullptr || length <= 0)
            {
                refused(ignore_errors, "no data");
                return Image();
            }

            // Huge thanks to https://gist.github.com/PhirePhly/3080633
            JpegDecode d;
            memset(&d, 0, sizeof(d));

            // An error in the headers gives no picture: the size they give cannot be trusted
            if (!read_headers(&d, data, length))
            {
                jpeg_destroy_decompress(&d.cinfo);
                refused(ignore_errors, d.message);
                return Image();
            }

            // Fork: the size is checked before memory is set aside for it (libjpeg has checked
            // that both are 1 to 65500, so the size_t product cannot overflow)
            size_t width = d.cinfo.image_width, height = d.cinfo.image_height;
            if (width * height > max_pixels)
            {
                jpeg_destroy_decompress(&d.cinfo);
                logger->warn("JPEG of %zu x %zu (%zu pixels) is over the limit of %zu pixels, not decoded", width, height, width * height, max_pixels);
                return Image();
            }

            Image img;
            try
            {
                img = Image(8, width, height, 1); // all 0
            }
            catch (const std::exception &e)
            {
                jpeg_destroy_decompress(&d.cinfo);
                logger->warn("JPEG of %zu x %zu: %s", width, height, e.what());
                return Image();
            }

            bool complete = decode_rows(&d, (uint8_t *)img.raw_data(), width, height);
            jpeg_destroy_decompress(&d.cinfo);
            if (complete)
                return img;
            if (!ignore_errors)
            {
                refused(ignore_errors, d.message);
                return Image();
            }

            // Fork: with ignore_errors, the rows decoded before the error are kept and the rest
            // set to 0, the row the error came in too (it may be partly written)
            memset((uint8_t *)img.raw_data() + d.rows_done * width, 0, (height - d.rows_done) * width);
            logger->warn("JPEG damaged, kept %zu of %zu rows: %s", d.rows_done, height, d.message);
            return img;
        }

        /*Image<uint16_t> decompress_j2k_openjp2(uint8_t *data, int length)
        {
            Image<uint16_t> img;

            // Init decoder parameters
            opj_dparameters_t core;
            memset(&core, 0, sizeof(opj_dparameters_t));
            opj_set_default_decoder_parameters(&core);

            // Set input buffer info struct
            opj_buffer_info bufinfo;
            bufinfo.buf = data;
            bufinfo.cur = data;
            bufinfo.len = length;

            // Setup image, stream and codec
            opj_image_t *image = NULL;
            opj_stream_t *l_stream = opj_stream_create_buffer_stream(&bufinfo, true);
            opj_codec_t *l_codec = opj_create_decompress(OPJ_CODEC_J2K);

            // Check we could open the stream
            if (!l_stream)
            {
                opj_destroy_codec(l_codec);
                return img;
            }

            // Setup decoder
            if (!opj_setup_decoder(l_codec, &core))
            {
                opj_stream_destroy(l_stream);
                opj_destroy_codec(l_codec);
                return img;
            }

            // Read header
            if (!opj_read_header(l_stream, l_codec, &image))
            {
                opj_stream_destroy(l_stream);
                opj_destroy_codec(l_codec);
                opj_image_destroy(image);
                return img;
            }

            // Decode image
            if (!(opj_decode(l_codec, l_stream, image) &&
                  opj_end_decompress(l_codec, l_stream)))
            {
                opj_destroy_codec(l_codec);
                opj_stream_destroy(l_stream);
                opj_image_destroy(image);
                return img;
            }

            // Parse into CImg
            img = Image<uint16_t>(image->x1, image->y1, 1);
            for (int i = 0; i < int(image->x1 * image->y1); i++)
                img[i] = image->comps[0].data[i];

            // Free everything up
            opj_destroy_codec(l_codec);
            opj_stream_destroy(l_stream);
            opj_image_destroy(image);

            return img;
        }*/
    } // namespace image
} // namespace satdump