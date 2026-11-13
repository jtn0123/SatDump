// Fork: gain-mode button selection uses the values written by clicks and saved settings.
#include "common/dsp_source_sink/gain_mode.h"
#include "doctest.h"
#include <initializer_list>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <string>

TEST_CASE("SDR gain modes: each button selects exactly its saved value")
{
    using namespace satdump::gain_mode;
    CHECK(static_cast<int>(SENSITIVE) == 0);
    CHECK(static_cast<int>(LINEAR) == 1);
    CHECK(static_cast<int>(MANUAL) == 2);
    CHECK(value(SENSITIVE) == 0);
    CHECK(value(LINEAR) == 1);
    CHECK(value(MANUAL) == 2);
    for (int current : {-1, 0, 1, 2, 3, 22})
    {
        CHECK(selected(current, SENSITIVE) == (current == 0));
        CHECK(selected(current, LINEAR) == (current == 1));
        CHECK(selected(current, MANUAL) == (current == 2));
    }
}

TEST_CASE("SDR gain modes: real Manual buttons use shared selection and click value")
{
    // Fork: device headers and ImGui need hardware/UI setup. Guard the real call sites as
    // well as testing the pure helper, so either plugin reverting to a literal fails here.
    for (const char *plugin : {"airspy", "hydrasdr"})
    {
        CAPTURE(plugin);
        const std::string name(plugin);
        std::ifstream input(std::string(SATDUMP_TEST_SOURCE_DIR) + "/plugins/sdr_sources/" +
                            name + "_sdr_support/" + name + "_sdr.cpp");
        REQUIRE(input.good());
        std::string source((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        source.erase(std::remove_if(source.begin(), source.end(), [](unsigned char c) { return std::isspace(c); }), source.end());
        const std::string button = "if(RImGui::RadioButton(\"Manual\",satdump::gain_mode::selected(gain_type,satdump::gain_mode::MANUAL))){"
                                   "gain_type=satdump::gain_mode::value(satdump::gain_mode::MANUAL);";
        CHECK(source.find(button) != std::string::npos);
        CHECK(source.find("gain_type==22") == std::string::npos);
    }
}
