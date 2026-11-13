// Fork: the real source must be safe to stop before a device is opened.
#include "doctest.h"
#include "../../plugins/sdr_sources/sddc_sdr_support/sddc_sdr.h"

TEST_CASE("SDDC source stops before open and twice")
{
    SDDCSource source({"sddc", "test", "0"});
    CHECK_NOTHROW(source.stop());
    CHECK_NOTHROW(source.stop());
}
