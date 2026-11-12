// Fork: the float code of the differential tests' own twins (twins_selftest.cpp). diff.cmake
// compiles this file twice: with -ffp-contract=off as the float rule's default says (diff.h,
// rule 1: the *_strict functions), and with the project's own flags, as SatDump's C++ is built
// today (the *_contracted functions), where GCC may fuse a * b + c into one multiply-add with
// a single rounding. rust/tests/difftest/toys.rs has the Rust versions, which never fuse.
#include "diff.h"

#define SD_FLOATS_NAME2(name, build) name##_##build
#define SD_FLOATS_NAME(name, build) SD_FLOATS_NAME2(name, build)

namespace satdump::diff
{
    // A one-pole low-pass filter: float in, float out
    float SD_FLOATS_NAME(lowpass, SD_FLOATS_BUILD)(float y, const uint8_t *in, size_t n, float *out)
    {
        for (size_t i = 0; i < n; i++)
        {
            const float x = in[i] / 255.0f;
            y = 0.3f * x + 0.7f * y;
            out[i] = y;
        }
        return y;
    }

    // The Meteor LRPT reader's repair of missing lines (plugins/meteor_support/meteor/
    // instruments/msumr/lrpt_msumr_reader.cpp, getChannel): the pixels between a good one
    // (top) and the next good one `range` lines below (bottom), stored as Image::set(size_t,
    // int) stores them: the float made an int, then 8 bits. An integer out of a float.
    void SD_FLOATS_NAME(gapfill, SD_FLOATS_BUILD)(uint8_t top, uint8_t bottom, size_t range, uint8_t *out)
    {
        for (size_t fix_y = 1; fix_y < range; fix_y++)
        {
            const float percent = (float)fix_y / (float)range;
            out[fix_y - 1] = uint8_t(int((1.0f - percent) * (float)top + percent * (float)bottom));
        }
    }
} // namespace satdump::diff
