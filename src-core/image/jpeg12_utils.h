#pragma once

/**
 * @file jpeg12_utils.h
 * @brief Utilities specific to 12-bit JPEG manipulation (from memory)
 */

#include "image.h"
#include "jpeg_utils.h" // Fork: JPEG_MAX_PIXELS
#include <cstddef>
#include <cstdint>

namespace satdump
{
    namespace image
    {
        /**
         * @brief Decompress 12-bit grayscale JPEG Data from memory
         * @param data input buffer
         * @param length buffer size
         * @param ignore_errors if true, a picture libjpeg finds an error in once its headers
         * are read gives the rows decoded before the error, the rest left 0. Without it, or when
         * the error is in the headers (whose picture size cannot then be trusted), there is no
         * picture (an empty image): with ignore_errors a warning gives libjpeg's reason, without
         * it a debug line.
         * @param max_pixels a header claiming a bigger picture gives an empty image
         * @return the image (16 bits, the 12-bit samples in the top bits, 1 channel; for a
         * YCbCr colour stream its luminance, the first component; RGB and CMYK streams are
         * refused)
         */
        Image decompress_jpeg12(uint8_t *data, int length, bool ignore_errors = false, size_t max_pixels = JPEG_MAX_PIXELS);
    } // namespace image
} // namespace satdump