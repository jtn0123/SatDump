#include "j2k_stream.h"
#include "logger.h"
#include <cstring>
#include <memory>

namespace satdump
{
    namespace image
    {
        namespace
        {
            size_t buffer_left(const J2KBuffer *b) { return b->pos < b->size ? b->size - b->pos : 0; }

            std::string trimmed(const char *msg)
            {
                std::string s = msg == nullptr ? "" : msg;
                while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
                    s.pop_back();
                return s;
            }

            void on_error(const char *msg, void *client_data)
            {
                J2KMessages *m = (J2KMessages *)client_data;
                if (m->errors++ == 0)
                    m->error = trimmed(msg);
            }

            void on_warning(const char *msg, void *client_data)
            {
                J2KMessages *m = (J2KMessages *)client_data;
                if (m->warnings++ == 0)
                    m->warning = trimmed(msg);
            }

            struct CodecDeleter
            {
                void operator()(opj_codec_t *codec) const { opj_destroy_codec(codec); }
            };

            struct ImageDeleter
            {
                void operator()(opj_image_t *image) const { opj_image_destroy(image); }
            };
        } // namespace

        void j2k_capture_messages(opj_codec_t *codec, J2KMessages &messages)
        {
            opj_set_error_handler(codec, on_error, &messages);
            opj_set_warning_handler(codec, on_warning, &messages);
        }

        OPJ_SIZE_T j2k_buffer_read(void *dst, OPJ_SIZE_T nbytes, void *user_data)
        {
            J2KBuffer *b = (J2KBuffer *)user_data;
            size_t n = buffer_left(b);
            if (n == 0)
                return (OPJ_SIZE_T)-1;
            if (nbytes < n)
                n = nbytes;
            if (n > 0)
                memcpy(dst, b->data + b->pos, n);
            b->pos += n;
            return n;
        }

        OPJ_OFF_T j2k_buffer_skip(OPJ_OFF_T nbytes, void *user_data)
        {
            J2KBuffer *b = (J2KBuffer *)user_data;
            size_t n = buffer_left(b);
            if (n == 0 || nbytes < 0)
                return -1;
            if ((uint64_t)nbytes < n)
                n = (size_t)nbytes;
            b->pos += n;
            return (OPJ_OFF_T)n;
        }

        OPJ_BOOL j2k_buffer_seek(OPJ_OFF_T position, void *user_data)
        {
            J2KBuffer *b = (J2KBuffer *)user_data;
            if (position < 0 || (uint64_t)position > b->size)
                b->pos = b->size;
            else
                b->pos = (size_t)position;
            return OPJ_TRUE;
        }

        opj_stream_t *j2k_create_buffer_stream(J2KBuffer *buffer)
        {
            if (buffer == nullptr || (buffer->data == nullptr && buffer->size > 0))
                return nullptr;
            if (buffer->pos > buffer->size)
                buffer->pos = buffer->size;

            opj_stream_t *stream = opj_stream_create(OPJ_J2K_STREAM_CHUNK_SIZE, OPJ_TRUE);
            if (stream == nullptr)
                return nullptr;
            opj_stream_set_user_data(stream, buffer, nullptr);
            opj_stream_set_user_data_length(stream, buffer->size);
            opj_stream_set_read_function(stream, j2k_buffer_read);
            opj_stream_set_skip_function(stream, j2k_buffer_skip);
            opj_stream_set_seek_function(stream, j2k_buffer_seek);
            return stream;
        }

        opj_image_t *j2k_decode(opj_stream_t *stream, std::string &error, std::string &warning, uint64_t max_samples)
        {
            error.clear();
            warning.clear();
            if (stream == nullptr)
            {
                error = "could not open the input";
                return nullptr;
            }

            opj_dparameters_t parameters;
            memset(&parameters, 0, sizeof(opj_dparameters_t));
            opj_set_default_decoder_parameters(&parameters);

            std::unique_ptr<opj_codec_t, CodecDeleter> codec(opj_create_decompress(OPJ_CODEC_J2K));
            if (!codec)
            {
                error = "could not create the decoder";
                return nullptr;
            }

            // Fork: OpenJPEG's own messages give the reason (they were dropped before)
            J2KMessages messages;
            j2k_capture_messages(codec.get(), messages);

            if (!opj_setup_decoder(codec.get(), &parameters))
            {
                error = messages.reason("could not set up the decoder");
                return nullptr;
            }

            opj_image_t *header = nullptr;
            if (!opj_read_header(stream, codec.get(), &header))
            {
                opj_image_destroy(header);
                error = messages.reason("could not read the header");
                return nullptr;
            }
            std::unique_ptr<opj_image_t, ImageDeleter> image(header);

            if (image->numcomps == 0 || image->comps == nullptr || image->x1 <= image->x0 || image->y1 <= image->y0)
            {
                error = "the header describes an empty picture";
                return nullptr;
            }
            const uint64_t samples = (uint64_t)(image->x1 - image->x0) * (image->y1 - image->y0) * image->numcomps;
            if (max_samples > 0 && samples > max_samples)
            {
                error = "the picture is larger than allowed (" + std::to_string(samples) + " samples)";
                return nullptr;
            }

            if (!(opj_decode(codec.get(), stream, image.get()) && opj_end_decompress(codec.get(), stream)))
            {
                error = messages.reason("could not decode the picture");
                return nullptr;
            }

            // Fork: the decoded data of a component is w x h samples, which is less than x1 x y1
            // (what was read before) when the picture does not start at 0,0 or is subsampled
            const opj_image_comp_t &first = image->comps[0];
            if (first.data == nullptr || first.w == 0 || first.h == 0)
            {
                error = "the decoder gave no picture";
                return nullptr;
            }

            if (messages.warnings > 0)
                warning = messages.warning + (messages.warnings > 1 ? " (" + std::to_string(messages.warnings) + " warnings)" : "");
            return image.release();
        }

        Image j2k_first_component(const opj_image_t *decoded)
        {
            const opj_image_comp_t &comp = decoded->comps[0];
            const size_t samples = (size_t)comp.w * comp.h;
            Image img(16, comp.w, comp.h, 1);
            for (size_t i = 0; i < samples; i++)
                img.set(i, comp.data[i]);
            return img;
        }

        bool j2k_all_components(const opj_image_t *decoded, Image &img, std::string &error)
        {
            // Fork: a precision above 16 bits was shifted by a negative amount (undefined)
            const int depth = decoded->comps[0].prec;
            if (depth < 1 || depth > 16)
            {
                error = "unsupported bit depth " + std::to_string(depth) + " (1 to 16 are supported)";
                return false;
            }

            // Fork: SatDump keeps every channel at one size, so a subsampled component (fewer
            // samples than the first; more were read before) is refused
            const opj_image_comp_t &first = decoded->comps[0];
            for (uint32_t c = 1; c < decoded->numcomps; c++)
            {
                const opj_image_comp_t &comp = decoded->comps[c];
                if (comp.data == nullptr || comp.w != first.w || comp.h != first.h)
                {
                    error = "component " + std::to_string(c) + " is not the size of the first one (subsampled pictures are not supported)";
                    return false;
                }
            }

            const int bits = depth > 8 ? 16 : 8;
            const int shift = bits - depth;
            const size_t samples = (size_t)first.w * first.h;
            img.init(bits, first.w, first.h, decoded->numcomps);
            for (uint32_t c = 0; c < decoded->numcomps; c++)
            {
                // Fork: shifted as unsigned (a negative value shifted left is undefined); the image
                // keeps the low 8 or 16 bits as before
                const OPJ_INT32 *data = decoded->comps[c].data;
                for (size_t i = 0; i < samples; i++)
                    img.set(c, i, (int)(((uint32_t)data[i] << shift) & 0xFFFF));
            }
            return true;
        }

        Image j2k_decompress_buffer(const uint8_t *data, size_t length, uint64_t max_samples)
        {
            J2KBuffer buffer;
            buffer.data = data;
            buffer.size = length;
            opj_stream_t *stream = j2k_create_buffer_stream(&buffer);

            std::string error, warning;
            opj_image_t *decoded = j2k_decode(stream, error, warning, max_samples);
            if (stream != nullptr)
                opj_stream_destroy(stream);

            if (decoded == nullptr)
            {
                logger->warn("J2K: could not decompress %zu bytes of data: %s", length, error.c_str());
                return Image();
            }
            if (!warning.empty())
                logger->warn("J2K: %zu bytes of data decompressed with problems: %s", length, warning.c_str());

            Image img = j2k_first_component(decoded);
            opj_image_destroy(decoded);
            return img;
        }
    } // namespace image
} // namespace satdump
