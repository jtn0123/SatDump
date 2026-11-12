// Fork: pass prediction (getPassesForSatellite) must take the station height in metres, as the
// orbit library (predict) does. It multiplied the height by DEG_TO_RAD, so a station at 2000 m
// was computed as if it were at 35 m. Uses made-up orbit elements in a database held in memory.

#include "common/geodetic/geodetic_coordinates.h"
#include "common/tracking/scheduler/passes.h"
#include "common/tracking/tle.h"
#include "doctest.h"
#include "libs/predict/predict.h"
#include "offline_rig.h"
#include <cmath>
#include <memory>

namespace
{
    // Made-up elements in the shape of MetOp-B's (sun-synchronous, about 820 km up)
    satdump::TLE metop_like()
    {
        satdump::TLE tle;
        tle.norad = 38771;
        tle.name = "METOP-B";
        tle.line1 = "1 38771U 12049A   25314.50000000  .00000100  00000-0  50000-4 0  9992";
        tle.line2 = "2 38771  98.6900  10.1234 0001500  90.1000 270.0500 14.21512345678909";
        tle.time = 0;
        return tle;
    }

    // The highest elevation, in degrees, seen at 51 even steps from `aos` to `los` (as
    // getPassesForSatellite samples a pass) from a station `alt_m` metres up
    float max_elevation(const satdump::TLE &tle, double aos, double los, double lat, double lon, double alt_m)
    {
        std::unique_ptr<predict_observer_t, decltype(&predict_destroy_observer)> obs(predict_create_observer("Test", lat * DEG_TO_RAD, lon * DEG_TO_RAD, alt_m), &predict_destroy_observer);
        std::unique_ptr<predict_orbital_elements_t, decltype(&predict_destroy_orbital_elements)> sat(predict_parse_tle(tle.line1.c_str(), tle.line2.c_str()), &predict_destroy_orbital_elements);
        REQUIRE(obs != nullptr);
        REQUIRE(sat != nullptr);

        float max_el = 0;
        double step = std::fabs(los - aos) / 50.0;
        for (double t = aos; t <= los; t += step)
        {
            predict_position pos;
            predict_observation seen;
            predict_orbit(sat.get(), &pos, predict_to_julian_double(t));
            predict_observe_orbit(obs.get(), &pos, &seen);
            if (seen.elevation * RAD_TO_DEG > max_el)
                max_el = seen.elevation * RAD_TO_DEG;
        }
        return max_el;
    }
} // namespace

TEST_CASE("Passes: the station height is used in metres")
{
    test_rig::OfflineRig rig(test_rig::general("Never", false));
    satdump::KeplerData kep;
    REQUIRE(satdump::tleToKepler(metop_like(), kep));
    rig.keplers->putKepler(kep);
    auto tle = satdump::db_keplers->get_from_norad(38771);
    REQUIRE(tle.has_value());

    const double lat = 45, lon = 10, alt = 2000;

    // A pass window given to the function (the "premade" passes): it only fills in the
    // highest elevation, seen from the station. 1000 s, so the 50 steps are whole seconds
    std::vector<satdump::SatellitePass> found = satdump::getPassesForSatellite(38771, kep.epoch, 86400, lon, lat, alt);
    REQUIRE(found.size() > 0);
    const double aos = found[0].aos_time, los = aos + 1000;
    std::vector<satdump::SatellitePass> premade = satdump::getPassesForSatellite(38771, 0, 0, lon, lat, alt, {{38771, aos, los, 0}});
    REQUIRE(premade.size() == 1);

    const float at_2000m = max_elevation(*tle, aos, los, lat, lon, alt);
    const float at_35m = max_elevation(*tle, aos, los, lat, lon, alt * DEG_TO_RAD);
    // The two heights give elevations far enough apart for the checks below to tell them apart
    REQUIRE(std::fabs(at_2000m - at_35m) > 0.001);
    CHECK(premade[0].max_elevation == doctest::Approx(at_2000m).epsilon(1e-6));
    CHECK(premade[0].max_elevation != doctest::Approx(at_35m).epsilon(1e-6));

    // The passes it finds itself use the same height
    const float found_2000m = max_elevation(*tle, found[0].aos_time, found[0].los_time, lat, lon, alt);
    const float found_35m = max_elevation(*tle, found[0].aos_time, found[0].los_time, lat, lon, alt * DEG_TO_RAD);
    REQUIRE(std::fabs(found_2000m - found_35m) > 0.001);
    CHECK(std::fabs(found[0].max_elevation - found_2000m) < 1e-4);
}
