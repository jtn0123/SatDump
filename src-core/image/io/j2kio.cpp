#include "../io.h"
#include "../j2k_stream.h"
#include "logger.h"
#include <cstring>
#include <filesystem>
#include <openjpeg.h>
#include <vector>

#define INVERT_ENDIAN_16(x) ((x >> 8) | (x << 8))

namespace satdump
{
    namespace image
    {
        void save_j2k(Image &img, std::string file)
        {
            auto d_depth = img.depth();
            auto d_channels = img.channels();
            auto d_height = img.height();
            auto d_width = img.width();

            if (img.size() == 0 || d_height == 0) // Make sure we aren't just gonna crash
            {
                logger->trace("Tried to save empty J2K!");
                return;
            }

            opj_cparameters_t parameters;
            opj_image_t *image;
            OPJ_CODEC_FORMAT codec_format = OPJ_CODEC_J2K;

            opj_set_default_encoder_parameters(&parameters);
            parameters.numresolution = 5;
            // Fork: OpenJPEG needs each side to be at least 2^(resolutions - 1) pixels, so a
            // picture under 16 pixels wide or high (which could not be saved) gets fewer levels
            while (parameters.numresolution > 1 && (d_width < (1u << (parameters.numresolution - 1)) || d_height < (1u << (parameters.numresolution - 1))))
                parameters.numresolution--;

            {
                // Fork: one entry per channel. There were always 4, so a picture with more
                // channels was written past the end of the array (on the stack)
                std::vector<opj_image_cmptparm_t> comp_params(d_channels);
                memset(comp_params.data(), 0, comp_params.size() * sizeof(opj_image_cmptparm_t));

                for (int i = 0; i < d_channels; i++)
                {
                    comp_params[i].prec = d_depth;
                    // comp_params[i].bpp = 8;
                    comp_params[i].sgnd = 0;
                    comp_params[i].dx = parameters.subsampling_dx;
                    comp_params[i].dy = parameters.subsampling_dy;
                    comp_params[i].w = d_width;
                    comp_params[i].h = d_height;
                }

                image = opj_image_create(d_channels, comp_params.data(), d_channels == 1 ? OPJ_CLRSPC_GRAY : OPJ_CLRSPC_SRGB);

                if (image != NULL)
                {
                    image->x0 = 0;
                    image->y0 = 0;
                    image->x1 = (d_width - 1) * parameters.subsampling_dx + 1;
                    image->y1 = (d_height - 1) * parameters.subsampling_dy + 1;

                    // Put into image
                    for (int c = 0; c < d_channels; c++)
                        for (size_t i = 0; i < d_width * d_height; i++)
                            image->comps[c].data[i] = img.get(c, i);
                }
            }

            if (image == NULL)
            {
                logger->error("Could not save J2K image %s: OpenJPEG could not create the picture", file.c_str());
                return;
            }

            opj_stream_t *stream;
            opj_codec_t *codec;
            // Fork: OpenJPEG's own messages give the reason for a failure (they were dropped)
            J2KMessages messages;

            stream = NULL;
            // Fork: the colour transform works on the first three components, so it is only used
            // when there are three or more. It was on for two, and OpenJPEG then read a third
            // component that does not exist (a crash)
            parameters.tcp_mct = image->numcomps >= 3 ? 1 : 0;

            codec = opj_create_compress(codec_format);

            if (codec == NULL)
            {
                logger->error("Could not save J2K image %s: could not create the encoder", file.c_str());
                goto abort;
            }

            j2k_capture_messages(codec, messages);
            if (!opj_setup_encoder(codec, &parameters, image))
            {
                logger->error("Could not save J2K image %s: %s", file.c_str(), messages.reason("could not set up the encoder").c_str());
                goto abort;
            }

            stream = opj_stream_create_default_file_stream(file.c_str(), false);

            if (stream == NULL)
            {
                logger->error("Could not save J2K image %s: could not open the file", file.c_str());
                goto abort;
            }

            if (!opj_start_compress(codec, image, stream))
            {
                logger->error("Could not save J2K image %s: %s", file.c_str(), messages.reason("could not start compressing").c_str());
                goto abort;
            }

            if (!opj_encode(codec, stream))
            {
                logger->error("Could not save J2K image %s: %s", file.c_str(), messages.reason("could not compress").c_str());
                goto abort;
            }

            if (!opj_end_compress(codec, stream))
                logger->error("Could not save J2K image %s: %s", file.c_str(), messages.reason("could not finish the file").c_str());

        abort:

            if (stream)
                opj_stream_destroy(stream);

            if (codec)
                opj_destroy_codec(codec);

            opj_image_destroy(image);

            if (parameters.cp_comment)
                free(parameters.cp_comment);

            return;
        }

        void load_j2k(Image &img, std::string file)
        {
            if (!std::filesystem::exists(file))
                return;

            // Fork: a file that cannot be read or decoded is logged with OpenJPEG's reason (it was
            // dropped silently), and a decoded picture is checked before it is copied: the copy
            // read x1 x y1 samples of every component, which is more than OpenJPEG decodes when
            // the picture does not start at 0,0 or is subsampled, and shifted by a negative
            // amount above 16 bits
            opj_stream_t *stream = opj_stream_create_file_stream(file.c_str(), OPJ_J2K_STREAM_CHUNK_SIZE, true);
            std::string error, warning;
            opj_image_t *decoded = j2k_decode(stream, error, warning);
            if (stream != nullptr)
                opj_stream_destroy(stream);

            if (decoded == nullptr)
            {
                logger->error("Could not load J2K image %s: %s", file.c_str(), error.c_str());
                return;
            }
            if (!warning.empty())
                logger->warn("J2K image %s loaded with problems: %s", file.c_str(), warning.c_str());

            if (!j2k_all_components(decoded, img, error))
                logger->error("Could not load J2K image %s: %s", file.c_str(), error.c_str());
            opj_image_destroy(decoded);
        }
    } // namespace image
} // namespace satdump
