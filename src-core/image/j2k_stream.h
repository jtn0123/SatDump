#pragma once

/**
 * @file j2k_stream.h
 * @brief OpenJPEG helpers shared by the J2K loaders: a stream that reads from memory,
 * decoding with the reason for a failure, and the conversion of a decoded picture
 */

#include "image/image.h"
#include <cstddef>
#include <cstdint>
#include <openjpeg.h>
#include <string>

namespace satdump
{
    namespace image
    {
        // Fork: the memory buffer an OpenJPEG input stream reads from. OpenJPEG has no
        // memory stream of its own; the bundled copy had a SatDump-only one
        // (opj_stream_create_buffer_stream), which the system library does not have.
        struct J2KBuffer
        {
            const uint8_t *data = nullptr; // the codestream
            size_t size = 0;               // its length in bytes
            size_t pos = 0;                // the next byte to read, never past size
        };

        // OpenJPEG's messages for one codec, kept for the log: the first error and warning
        struct J2KMessages
        {
            std::string error, warning;
            int errors = 0, warnings = 0;

            // "what: OpenJPEG's first error", or just "what" when there was none
            std::string reason(const std::string &what) const { return errors > 0 ? what + ": " + error : what; }
        };

        /**
         * @brief Send a codec's error and warning messages to messages (they are dropped otherwise)
         */
        void j2k_capture_messages(opj_codec_t *codec, J2KMessages &messages);

        /**
         * @brief Create an OpenJPEG input stream that reads a codestream from memory
         * (OpenJPEG's public stream API only). The buffer must outlive the stream.
         * @param buffer the codestream, read from its pos
         * @return the stream (free it with opj_stream_destroy), or nullptr
         */
        opj_stream_t *j2k_create_buffer_stream(J2KBuffer *buffer);

        // The stream's callbacks, public for the unit tests. They behave like the bundled
        // copy's: a read at the end returns (OPJ_SIZE_T)-1 and a skip at the end -1; a read
        // or skip past the end stops at the end and returns what it did; a seek past the end
        // (or before the start) goes to the end and succeeds. The bundled skip moved past the
        // end, so a later read copied bytes from after the buffer (OpenJPEG itself never asks
        // for that, as it knows the length); a negative skip is refused (-1), as OpenJPEG
        // never asks for one either.
        OPJ_SIZE_T j2k_buffer_read(void *dst, OPJ_SIZE_T nbytes, void *user_data);
        OPJ_OFF_T j2k_buffer_skip(OPJ_OFF_T nbytes, void *user_data);
        OPJ_BOOL j2k_buffer_seek(OPJ_OFF_T position, void *user_data);

        /**
         * @brief Decode a J2K codestream. Each component of the result holds comps[c].w x
         * comps[c].h samples (less than x1 x y1 when the picture does not start at 0,0 or is
         * subsampled).
         * @param stream input stream
         * @param error set to the reason when nullptr is returned
         * @param warning set to OpenJPEG's first warning (for example a cut-off codestream that
         * was still decoded), with the number of warnings, or left empty
         * @param max_samples refuse pictures with more samples (width x height x components)
         * than this before decoding them; 0 for no limit
         * @return the decoded picture (free it with opj_image_destroy), or nullptr
         */
        opj_image_t *j2k_decode(opj_stream_t *stream, std::string &error, std::string &warning, uint64_t max_samples = 0);

        /**
         * @brief The first component of a decoded picture, values as they are, in a 16-bit image
         * (what decompress_j2k_openjp2 returns)
         */
        Image j2k_first_component(const opj_image_t *decoded);

        /**
         * @brief Every component of a decoded picture, scaled to 8 bits (precision up to 8) or
         * 16 bits (what load_j2k gives). A precision above 16 bits, or components of different
         * sizes (subsampled), are refused, as SatDump keeps every channel at one size.
         * @param img set up and filled; left as it was when false is returned
         * @param error set to the reason when false is returned
         * @return whether the picture could be converted
         */
        bool j2k_all_components(const opj_image_t *decoded, Image &img, std::string &error);

        /**
         * @brief Decode a J2K codestream held in memory into a 16-bit image of its first
         * component. A codestream that cannot be decoded gives an empty image and a warning.
         * @param data codestream
         * @param length its length in bytes
         * @param max_samples see j2k_decode
         */
        Image j2k_decompress_buffer(const uint8_t *data, size_t length, uint64_t max_samples = 0);
    } // namespace image
} // namespace satdump
