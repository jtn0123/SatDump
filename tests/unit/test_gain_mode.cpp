// Fork: gain-mode button selection uses the values written by clicks and saved settings.
#include "common/dsp_source_sink/gain_mode.h"
#include "doctest.h"
#include <initializer_list>

TEST_CASE("SDR gain modes: each button selects exactly its saved value")
{
    using namespace satdump::gain_mode;
    CHECK(static_cast<int>(SENSITIVE) == 0);
    CHECK(static_cast<int>(LINEAR) == 1);
    CHECK(static_cast<int>(MANUAL) == 2);
    for (int current : {-1, 0, 1, 2, 3, 22})
    {
        CHECK(selected(current, SENSITIVE) == (current == 0));
        CHECK(selected(current, LINEAR) == (current == 1));
        CHECK(selected(current, MANUAL) == (current == 2));
    }
}
