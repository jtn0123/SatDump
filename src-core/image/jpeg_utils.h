#pragma once

/**
 * @file jpeg_utils.h
 * @brief Utilities specific to JPEG manipulation (from memory)
 */

#include "image.h"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace satdump
{
    namespace image
    {
        /**
         * @brief Fork: the largest picture decompress_jpeg and decompress_jpeg12 decode, in
         * pixels. A JPEG header can claim up to 65500 x 65500 (4.3 GB at 8 bits, 8.6 GB at 12),
         * which a damaged header easily does; the largest pictures SatDump's callers decode
         * (GK-2A, MSG and Elektro-L segments, PROBA SWAP, Hinode, MATS, DSCOVR EPIC) are under
         * 50 million pixels.
         */
        constexpr size_t JPEG_MAX_PIXELS = 100000000;

        /**
         * @brief Decompress 8-bit grayscale JPEG Data from memory
         * @param data input buffer
         * @param length buffer size
         * @param ignore_errors if true, a picture libjpeg finds an error in once its headers
         * are read gives the rows decoded before the error, the rest left 0 (needed for a few
         * satellites sending slightly malformed JPEGs). Without it, or when the error is in the
         * headers (whose picture size cannot then be trusted), there is no picture (an empty
         * image): with ignore_errors a warning gives libjpeg's reason, without it a debug line.
         * @param max_pixels a header claiming a bigger picture gives an empty image
         * @return the image (8 bits, 1 channel; for a YCbCr colour stream its luminance, the
         * first component; RGB and CMYK streams are refused)
         */
        Image decompress_jpeg(uint8_t *data, int length, bool ignore_errors = false, size_t max_pixels = JPEG_MAX_PIXELS);

        /**
         * @brief Compress JPEG to memory
         * @param img the image to compress (8-bit)
         * @return vector containing the JPEG stream
         */
        std::vector<uint8_t> save_jpeg_mem(Image &img);

        // Decompress J2K Data from memory with OpenJP2
        // Image<uint16_t> decompress_j2k_openjp2(uint8_t *data, int length);
    } // namespace image
} // namespace satdump