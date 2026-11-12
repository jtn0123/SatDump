#include "jpeg12_utils.h"
#include "logger.h"
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <exception>
#include <stdexcept>
extern "C"
{
#include "libs/jpeg12/jpeglib12.h"
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
            // Fork: the same as decompress_jpeg (jpeg_utils.cpp), with the 12-bit copy of
            // libjpeg. libjpeg reports an error by calling error_exit, which must not return.
            // With ignore_errors it used to return, and libjpeg carried on with data it had just
            // found to be bad, which it is not written to do. Now error_exit always longjmps
            // back, keeping libjpeg's message. Everything libjpeg and the jump use is in this
            // struct, which has no destructor and is not a local of the functions that call
            // setjmp, so the longjmp skips no C++ destructor and no value is lost to it.
            struct JpegDecode12
            {
                jpeg_error_mgr pub; // first: libjpeg passes &pub to error_exit, which is this struct
                jmp_buf jump;
                jpeg_decompress_struct cinfo;
                char message[JMSG_LENGTH_MAX];
                size_t rows_done;
            };

            void decode_error_exit12(j_common_ptr cinfo)
            {
                JpegDecode12 *d = (JpegDecode12 *)cinfo->err;
                cinfo->err->format_message(cinfo, d->message);
                longjmp(d->jump, 1);
            }

            // Sets up libjpeg and reads the headers; false (with d->message) on an error or when
            // there is no picture. d must be zeroed: jpeg_destroy_decompress is then safe after
            // any error.
            bool read_headers12(JpegDecode12 *d, uint8_t *data, int length)
            {
                if (setjmp(d->jump))
                    return false;
                d->cinfo.err = jpeg_std_error(&d->pub);
                d->pub.error_exit = decode_error_exit12; // warnings are still printed by libjpeg, as before
                jpeg_create_decompress(&d->cinfo);
                jpeg_mem__src12(&d->cinfo, data, (unsigned long)length);
                if (jpeg_read_header(&d->cinfo, FALSE) != JPEG_HEADER_OK)
                {
                    snprintf(d->message, sizeof(d->message), "no picture in the stream");
                    return false;
                }
                return true;
            }

            // Decodes the picture into rows (width samples each, height rows); false (with
            // d->message) on an error, after d->rows_done rows
            bool decode_rows12(JpegDecode12 *d, JSAMPLE *rows, size_t width, size_t height)
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

            // Fork: a stream given no picture: a warning with libjpeg's reason with ignore_errors
            // (the caller keeps whatever it gets), a debug line for a strict caller, which decides
            // itself (GK-2A then tries JPEG 2000)
            void refused12(bool ignore_errors, const char *reason)
            {
                if (ignore_errors)
                    logger->warn("12-bit JPEG not decoded, no picture: %s", reason);
                else
                    logger->debug("12-bit JPEG refused: %s", reason);
            }
        } // namespace

        Image decompress_jpeg12(uint8_t *data, int length, bool ignore_errors, size_t max_pixels)
        {
            // Fork: a length below 0 (from a caller's size calculation gone wrong) was given to
            // libjpeg as a huge unsigned one, and it read past the end of the data
            if (data == nullptr || length <= 0)
            {
                refused12(ignore_errors, "no data");
                return Image();
            }

            // Huge thanks to https://gist.github.com/PhirePhly/3080633
            JpegDecode12 d;
            memset(&d, 0, sizeof(d));

            // An error in the headers gives no picture: the size they give cannot be trusted.
            // Fork: libjpeg's own memory is released after an error too (it was left behind)
            if (!read_headers12(&d, data, length))
            {
                jpeg_destroy_decompress(&d.cinfo);
                refused12(ignore_errors, d.message);
                return Image();
            }

            // Fork: the size is checked before memory is set aside for it (libjpeg has checked
            // that both are 1 to 65500, so the size_t product cannot overflow)
            size_t width = d.cinfo.image_width, height = d.cinfo.image_height;
            if (width * height > max_pixels)
            {
                jpeg_destroy_decompress(&d.cinfo);
                logger->warn("12-bit JPEG of %zu x %zu (%zu pixels) is over the limit of %zu pixels, not decoded", width, height, width * height, max_pixels);
                return Image();
            }

            Image img;
            try
            {
                img = Image(16, width, height, 1); // all 0
            }
            catch (const std::exception &e)
            {
                jpeg_destroy_decompress(&d.cinfo);
                logger->warn("12-bit JPEG of %zu x %zu: %s", width, height, e.what());
                return Image();
            }

            // libjpeg writes its 12-bit samples (short) into the picture's 16-bit buffer
            uint16_t *samples = (uint16_t *)img.raw_data();
            bool complete = decode_rows12(&d, (JSAMPLE *)samples, width, height);
            jpeg_destroy_decompress(&d.cinfo);
            if (!complete && !ignore_errors)
            {
                refused12(ignore_errors, d.message);
                return Image();
            }
            if (!complete)
            {
                // Fork: with ignore_errors, the rows decoded before the error are kept and the
                // rest set to 0, the row the error came in too (it may be partly written)
                memset(samples + d.rows_done * width, 0, (height - d.rows_done) * width * sizeof(uint16_t));
                logger->warn("12-bit JPEG damaged, kept %zu of %zu rows: %s", d.rows_done, height, d.message);
            }

            // The 12 bits to the top of 16, as before: only the low 12 bits of each sample count
            for (size_t i = 0; i < width * height; i++)
                samples[i] = (uint16_t)(samples[i] << 4);
            return img;
        }
    } // namespace image
} // namespace satdump
