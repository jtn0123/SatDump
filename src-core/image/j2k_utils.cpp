#include "j2k_utils.h"
#include "j2k_stream.h"
#include "logger.h"

namespace satdump
{
    namespace image
    {
        Image decompress_j2k_openjp2(uint8_t *data, int length)
        {
            // Fork: a negative length (a packet shorter than its header) was read as a huge size,
            // past the end of the data, and a null pointer was read from; both are now refused.
            // A length of 0, which OpenJPEG already refused, gets the same warning
            if (data == nullptr || length <= 0)
            {
                logger->warn("J2K: no data to decompress (%d bytes)", length);
                return Image();
            }

            // Fork: decoded through SatDump's own memory stream (j2k_stream.h), as the system
            // OpenJPEG has none; a codestream that cannot be decoded is logged with OpenJPEG's
            // reason instead of being dropped silently
            return j2k_decompress_buffer(data, (size_t)length);
        }
    } // namespace image
} // namespace satdump
