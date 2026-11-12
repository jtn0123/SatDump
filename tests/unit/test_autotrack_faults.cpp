// Fork: the autotrack thread must survive a callback that throws, a pass list that runs empty,
// and must report each pass in its own events. Uses made-up geostationary satellites (NORAD 90000
// and up) in a database held in memory, so nothing is read from disk or the network.

#include "common/tracking/scheduler/scheduler.h"
#include "common/tracking/tle.h"
#include "core/config.h"
#include "core/plugin.h"
#include "db/db_handler.h"
#include "db/kepler/kepler_handler.h"
#include "doctest.h"
#include "init.h"
#include "libs/predict/predict.h"
#include "pipeline/pipeline.h"
#include "utils/time.h"
#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
    using namespace std::chrono_literals;

    // Same orbit for all, only the NORAD number (and so the checksums) differ
    satdump::TLE geo_tle(int norad)
    {
        satdump::TLE tle;
        tle.norad = norad;
        tle.name = "FORK TEST GEO " + std::to_string(norad);
        int i = norad - 90000;
        tle.line1 = "1 " + std::to_string(norad) + "U 16071A   26314.50000000 -.00000096  00000+0  00000+0 0  999" + std::to_string((9 + i) % 10);
        tle.line2 = "2 " + std::to_string(norad) + "   0.0303 102.7478 0001148 210.8693 248.6226  1.00272062 2337" + std::to_string(4 + i);
        tle.time = 0;
        return tle;
    }

    template <typename F>
    bool wait_for(F cond, std::chrono::milliseconds limit)
    {
        auto start = std::chrono::steady_clock::now();
        while (!cond())
        {
            if (std::chrono::steady_clock::now() - start > limit)
                return false;
            std::this_thread::sleep_for(5ms);
        }
        return true;
    }

    // Sets up what the scheduler needs (settings, a pipeline for the pipeline selectors, the
    // Kepler database) and puts everything back afterwards. Declare it before the scheduler, so
    // the scheduler is gone first
    struct AutotrackRig
    {
        std::shared_ptr<satdump::KeplerDBHandler> old_keplers;
        nlohmann::ordered_json old_cfg;
        bool added_pipeline = false;
        std::shared_ptr<satdump::DBHandler> dbh;
        double qth_lon = 0;

        AutotrackRig(std::vector<int> norads)
        {
            if (!satdump::eventBus)
                satdump::eventBus = std::make_shared<satdump::EventBus>();

            old_cfg = satdump::satdump_cfg.main_cfg;
            satdump::satdump_cfg.main_cfg["satdump_general"]["kepler_update_interval"]["value"] = "Never";

            if (satdump::pipeline::pipelines.size() == 0)
            {
                satdump::pipeline::Pipeline p;
                p.id = "fork_autotrack_test";
                p.name = "Fork autotrack test";
                p.live = false;
                satdump::pipeline::pipelines.push_back(p);
                added_pipeline = true;
            }

            dbh = std::make_shared<satdump::DBHandler>(":memory:");
            dbh->init();
            auto keplers = std::make_shared<satdump::KeplerDBHandler>(dbh);
            keplers->init();
            for (int norad : norads)
            {
                satdump::KeplerData kep;
                REQUIRE(satdump::tleToKepler(geo_tle(norad), kep));
                keplers->putKepler(kep);
            }
            old_keplers = satdump::db_keplers;
            satdump::db_keplers = keplers;

            // A station 10 degrees east of the satellite, on the equator: always in view
            satdump::TLE tle = geo_tle(norads[0]);
            predict_orbital_elements_t *elements = predict_parse_tle(tle.line1.c_str(), tle.line2.c_str());
            REQUIRE(elements != nullptr);
            predict_position pos;
            predict_orbit(elements, &pos, predict_to_julian_double(satdump::getTime()));
            predict_destroy_orbital_elements(elements);
            qth_lon = pos.longitude * 180.0 / 3.14159265358979323846 + 10;
            if (qth_lon > 180)
                qth_lon -= 360;
        }

        std::vector<satdump::TrackedObject> tracked(std::vector<int> norads)
        {
            std::vector<satdump::TrackedObject> objs;
            for (int norad : norads)
            {
                satdump::TrackedObject obj;
                obj.norad = norad;
                objs.push_back(obj);
            }
            return objs;
        }

        ~AutotrackRig()
        {
            satdump::db_keplers = old_keplers;
            if (added_pipeline)
                satdump::pipeline::pipelines.clear();
            satdump::satdump_cfg.main_cfg = old_cfg;
        }
    };

    // Engages or disengages from another thread, so a lock that was never let go fails the test
    // instead of hanging it
    bool set_engaged_in_time(satdump::AutoTrackScheduler &sched, bool v, double t)
    {
        std::shared_ptr<std::atomic<bool>> done = std::make_shared<std::atomic<bool>>(false);
        std::thread th(
            [&sched, v, t, done]()
            {
                sched.setEngaged(v, t);
                *done = true;
            });
        bool in_time = wait_for([&]() { return (bool)*done; }, 5000ms);
        if (in_time)
            th.join();
        else
            th.detach(); // Cannot be recovered; the test has failed already
        return in_time;
    }

    std::mutex aos_events_mtx;
    std::vector<int> aos_events;
} // namespace

TEST_CASE("Autotrack: a callback that throws at AOS is logged, and autotrack carries on")
{
    std::atomic<int> aos_calls(0);
    AutotrackRig rig({90000});
    satdump::AutoTrackScheduler sched;
    sched.aos_callback = [&](satdump::AutoTrackCfg, satdump::SatellitePass, satdump::TrackedObject)
    {
        aos_calls++;
        throw std::runtime_error("AOS callback failure on purpose"); // This used to end the program
    };
    sched.setQTH(rig.qth_lon, 0, 0);
    sched.setTracked(rig.tracked({90000}));
    sched.setEngaged(true, satdump::getTime());
    REQUIRE(sched.getEngaged());

    sched.start();
    CHECK(wait_for([&]() { return aos_calls > 0; }, 5000ms));
    std::this_thread::sleep_for(500ms);
    CHECK(aos_calls == 1);

    // The scheduler's lock was let go
    CHECK(set_engaged_in_time(sched, false, satdump::getTime()));
    CHECK(!sched.getEngaged());
}

TEST_CASE("Autotrack: when no pass follows the last one, nothing is engaged on a pass that does not exist")
{
    std::atomic<int> eng_calls(0), aos_calls(0), los_calls(0);
    AutotrackRig rig({90000});
    satdump::AutoTrackScheduler sched;
    sched.eng_callback = [&](satdump::AutoTrackCfg, satdump::SatellitePass, satdump::TrackedObject) { eng_calls++; };
    sched.aos_callback = [&](satdump::AutoTrackCfg, satdump::SatellitePass, satdump::TrackedObject) { aos_calls++; };
    // The satellite is gone from the database by LOS, so there is no next pass
    sched.los_callback = [&](satdump::AutoTrackCfg, satdump::SatellitePass, satdump::TrackedObject)
    {
        rig.dbh->run_sql("DELETE FROM kepler;");
        los_calls++;
    };
    sched.setQTH(rig.qth_lon, 0, 0);
    sched.setTracked(rig.tracked({90000}));

    // A pass that began 13 hours ago, and so ended an hour ago
    sched.setEngaged(true, satdump::getTime() - 13 * 3600);
    REQUIRE(sched.getEngaged());
    CHECK(eng_calls == 1);

    sched.start();
    CHECK(wait_for([&]() { return los_calls > 0; }, 5000ms));
    std::this_thread::sleep_for(500ms);
    CHECK(aos_calls == 1);
    CHECK(los_calls == 1);
    // The engage callback used to run again here, with a pass read past the end of the empty list
    CHECK(eng_calls == 1);
    CHECK(set_engaged_in_time(sched, false, satdump::getTime()));
}

TEST_CASE("Autotrack: in multi mode, each AOS event is for its own pass")
{
    static bool registered = false;
    if (!registered)
    {
        registered = true;
        if (!satdump::eventBus)
            satdump::eventBus = std::make_shared<satdump::EventBus>();
        satdump::eventBus->register_handler<satdump::events::TrackingSchedulerAOSEvent>(
            [](satdump::events::TrackingSchedulerAOSEvent evt)
            {
                std::lock_guard<std::mutex> lock(aos_events_mtx);
                aos_events.push_back(evt.pass.norad);
            });
    }
    {
        std::lock_guard<std::mutex> lock(aos_events_mtx);
        aos_events.clear();
    }

    std::atomic<int> aos_calls(0);
    AutotrackRig rig({90001, 90002});
    satdump::AutoTrackScheduler sched;
    sched.aos_callback = [&](satdump::AutoTrackCfg, satdump::SatellitePass, satdump::TrackedObject) { aos_calls++; };
    satdump::AutoTrackCfg cfg;
    cfg.multi_mode = true;
    sched.setAutoTrackCfg(cfg);
    sched.setQTH(rig.qth_lon, 0, 0);
    sched.setTracked(rig.tracked({90001, 90002}));
    sched.setEngaged(true, satdump::getTime());
    REQUIRE(sched.getEngaged());

    sched.start();
    CHECK(wait_for([&]() { return aos_calls >= 2; }, 5000ms));
    std::this_thread::sleep_for(300ms);
    CHECK(set_engaged_in_time(sched, false, satdump::getTime()));

    std::vector<int> events;
    {
        std::lock_guard<std::mutex> lock(aos_events_mtx);
        events = aos_events;
    }
    // Both events used to carry the first pass of the list
    CHECK(events.size() == 2);
    CHECK(std::set<int>(events.begin(), events.end()) == std::set<int>({90001, 90002}));
}
