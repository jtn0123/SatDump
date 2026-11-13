#pragma once

// Fork: Airspy and HydraSDR share these saved gain-mode values. Keep selection and
// click assignment together so a button cannot check a different value from the one it sets.
namespace satdump
{
    namespace gain_mode
    {
        enum Mode { SENSITIVE = 0, LINEAR = 1, MANUAL = 2 };
        constexpr bool selected(int current, Mode mode) { return current == static_cast<int>(mode); }
    }
}
