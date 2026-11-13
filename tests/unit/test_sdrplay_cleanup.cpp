// Fork: run actual discovery, failed startup and cleanup against a local SDK double.
#include "doctest.h"
#include "../../plugins/sdr_sources/sdrplay_sdr_support/sdrplay_sdr.h"
extern "C" void fake_sdrplay_reset();
extern "C" void fake_sdrplay_fail_selection();
extern "C" int fake_sdrplay_releases();
extern "C" int fake_sdrplay_invalid_releases();

TEST_CASE("SDRplay discovery releases its selection exactly once")
{
    sdrplay_api_path = SATDUMP_FAKE_SDRPLAY;
    for (int scenario = 0; scenario < 4; ++scenario)
    {
        fake_sdrplay_reset();
        auto descriptors = SDRPlaySource::getAvailableSources();
        REQUIRE(descriptors.size() == 1);
        {
            SDRPlaySource source(descriptors[0]);
            source.open();
            CHECK(fake_sdrplay_releases() == 1);
            if (scenario == 1)
                fake_sdrplay_fail_selection();
            if (scenario == 1 || scenario == 2)
                CHECK_THROWS(source.start());
            if (scenario != 3)
            {
                source.stop();
                source.stop();
            }
        }
        CHECK(fake_sdrplay_releases() == (scenario == 2 ? 2 : 1));
        CHECK(fake_sdrplay_invalid_releases() == 0);
    }
}
