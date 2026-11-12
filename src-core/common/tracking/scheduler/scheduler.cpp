#include "scheduler.h"
#include "common/utils.h"
#include "core/plugin.h"
#include "init.h"
#include "logger.h"
#include "utils/time.h"

namespace satdump
{
    namespace
    {
        // Fork: a callback or event handler that throws is logged and autotrack carries on with the
        // pass. The error used to leave the autotrack thread, which ended the whole program
        template <typename F>
        void run_guarded(const char *what, int norad, F f)
        {
            try
            {
                f();
            }
            catch (const std::exception &e)
            {
                logger->error("Autotrack: %s for NORAD %d failed: %s", what, norad, e.what());
            }
            catch (...)
            {
                logger->error("Autotrack: %s for NORAD %d failed with an unknown error", what, norad);
            }
        }
    } // namespace

    AutoTrackScheduler::AutoTrackScheduler()
    {
        auto &tle_registry = db_keplers->all_;
        if (tle_registry.size() > 0)
            has_tle = true;

        for (auto &tle : tle_registry)
            satoptions.push_back(tle.name);

        // Updates on registry updates
        eventBus->register_handler<TLEsUpdatedEvent>(
            [this](TLEsUpdatedEvent)
            {
                std::lock_guard<std::mutex> lock(upcoming_satellite_passes_mtx); // Fork: let go also on an error

                auto &tle_registry = db_keplers->all_;
                if (tle_registry.size() > 0)
                    has_tle = true;

                satoptions.clear();
                for (auto &tle : tle_registry)
                    satoptions.push_back(tle.name);
            });
    }

    AutoTrackScheduler::~AutoTrackScheduler()
    {
        if (backend_should_run)
        {
            backend_should_run = false;
            if (backend_thread.joinable())
                backend_thread.join();
        }
    }

    void AutoTrackScheduler::processAutotrack(double curr_time)
    {
        if (autotrack_engaged)
        {
            // Fork: a lock that is always let go, also when something below throws
            std::lock_guard<std::mutex> lock(upcoming_satellite_passes_mtx);

            if (autotrack_cfg.multi_mode)
            {
                bool update = false;

                for (int i = 0; i < (int)upcoming_satellite_passes_sel.size(); i++)
                {
                    auto &pass = upcoming_satellite_passes_sel[i];
                    if (curr_time > pass.aos_time)
                    {
                        if (vfo_mode_norads_vis.count(pass.norad) == 0)
                        {
                            vfo_mode_norads_vis.insert({pass.norad, pass});

                            logger->critical("AOS!!!!!!!!!!!!!! %d", pass.norad);
                            // Fork: the event is for this pass (it carried the first pass of the list)
                            run_guarded("AOS event", pass.norad, [&]() { eventBus->fire_event<events::TrackingSchedulerAOSEvent>({pass}); });
                            TrackedObject obj;
                            for (auto &v : enabled_satellites)
                                if (v.norad == pass.norad)
                                    obj = v;
                            run_guarded("AOS callback", pass.norad, [&]() { aos_callback(autotrack_cfg, pass, obj); });
                        }
                    }
                }

            recheck:
                for (auto &p : vfo_mode_norads_vis)
                {
                    if (curr_time > p.second.los_time)
                    {
                        logger->critical("LOS!!!!!!!!!!!!!! %d ", p.first);
                        // Fork: the event is for this pass (it carried the first pass of the list, which
                        // may be empty by now)
                        run_guarded("LOS event", p.first, [&]() { eventBus->fire_event<events::TrackingSchedulerLOSEvent>({p.second}); });
                        TrackedObject obj;
                        for (auto &v : enabled_satellites)
                            if (v.norad == p.first)
                                obj = v;
                        run_guarded("LOS callback", p.first, [&]() { los_callback(autotrack_cfg, p.second, obj); });

                        vfo_mode_norads_vis.erase(p.first);
                        update = true;
                        goto recheck;
                    }
                }

                // Fork: only when there is a next pass (with none, this read past the end of the list)
                if (update)
                    updateAutotrackPasses(curr_time);
                if (update && upcoming_satellite_passes_sel.size() > 0)
                {
                    TrackedObject obj;
                    for (auto &v : enabled_satellites)
                        if (v.norad == upcoming_satellite_passes_sel[0].norad)
                            obj = v;
                    run_guarded("engage callback", upcoming_satellite_passes_sel[0].norad, [&]() { eng_callback(autotrack_cfg, upcoming_satellite_passes_sel[0], obj); });
                }
            }
            else
            {
                if (upcoming_satellite_passes_sel.size() > 0)
                {
                    if (!autotrack_pass_has_started && curr_time > upcoming_satellite_passes_sel[0].aos_time)
                    {
                        logger->critical("AOS!!!!!!!!!!!!!! %d", upcoming_satellite_passes_sel[0].norad);
                        SatellitePass pass = upcoming_satellite_passes_sel[0];
                        run_guarded("AOS event", pass.norad, [&]() { eventBus->fire_event<events::TrackingSchedulerAOSEvent>({pass}); });
                        TrackedObject obj;
                        for (auto &v : enabled_satellites)
                            if (v.norad == pass.norad)
                                obj = v;
                        run_guarded("AOS callback", pass.norad, [&]() { aos_callback(autotrack_cfg, pass, obj); });
                        autotrack_pass_has_started = true;
                    }
                }

                // Fork: the list is checked before its first pass is read (once it was empty, this
                // read past its end, and the program crashed on the next check)
                if (upcoming_satellite_passes_sel.size() > 0 && curr_time > upcoming_satellite_passes_sel[0].los_time)
                {
                    if (autotrack_pass_has_started)
                    {
                        logger->critical("LOS!!!!!!!!!!!!!! %d", upcoming_satellite_passes_sel[0].norad);
                        SatellitePass pass = upcoming_satellite_passes_sel[0];
                        run_guarded("LOS event", pass.norad, [&]() { eventBus->fire_event<events::TrackingSchedulerLOSEvent>({pass}); });
                        TrackedObject obj;
                        for (auto &v : enabled_satellites)
                            if (v.norad == pass.norad)
                                obj = v;
                        run_guarded("LOS callback", pass.norad, [&]() { los_callback(autotrack_cfg, pass, obj); });
                    }
                    autotrack_pass_has_started = false;
                    updateAutotrackPasses(curr_time);

                    if (upcoming_satellite_passes_sel.size() > 0) // Fork: there may be no next pass
                    {
                        TrackedObject obj;
                        for (auto &v : enabled_satellites)
                            if (v.norad == upcoming_satellite_passes_sel[0].norad)
                                obj = v;
                        run_guarded("engage callback", upcoming_satellite_passes_sel[0].norad, [&]() { eng_callback(autotrack_cfg, upcoming_satellite_passes_sel[0], obj); });
                    }
                }
            }
        }
    }

    void AutoTrackScheduler::updateAutotrackPasses(double curr_time)
    {
        upcoming_satellite_passes_all.clear();

        for (TrackedObject &obj : enabled_satellites)
        {
#if 0
            if (obj.norad == 41105)
            {
                std::vector<satdump::SatellitePass> dump_passes;
                for (uint64_t ctime = curr_time; ctime < curr_time + 12 * 3600; ctime++)
                {
                    if ((ctime - 9 * 60) % (30 * 60) == 0)
                    {
                        dump_passes.push_back({0, ctime - 600, ctime, 0});
                    }
                }

                auto passes = getPassesForSatellite(41105, curr_time, 0, qth_lon, qth_lat, qth_alt, dump_passes);
                upcoming_satellite_passes_all.insert(upcoming_satellite_passes_all.end(), passes.begin(), passes.end());
            }
            else
#endif
            {
                auto passes = getPassesForSatellite(obj.norad, curr_time, 12 * 3600, qth_lon, qth_lat, qth_alt);
                for (auto &pass : passes)
                    if (pass.max_elevation > obj.min_elevation)
                        upcoming_satellite_passes_all.push_back(pass);
            }
        }

        upcoming_satellite_passes_all = filterPassesByElevation(upcoming_satellite_passes_all, autotrack_cfg.autotrack_min_elevation, 90); // TODO

        std::sort(upcoming_satellite_passes_all.begin(), upcoming_satellite_passes_all.end(), [](SatellitePass &el1, SatellitePass &el2) { return el1.aos_time < el2.aos_time; });

        upcoming_satellite_passes_sel.clear();

        if (autotrack_cfg.multi_mode)
            upcoming_satellite_passes_sel = upcoming_satellite_passes_all;
        else
            upcoming_satellite_passes_sel = selectPassesForAutotrack(upcoming_satellite_passes_all);

        // for (auto ppp : upcoming_satellite_passes_sel)
        // logger->debug("Pass of %s at AOS %s LOS %s elevation %.2f",
        //               general_tle_registry->get_from_norad(ppp.norad).value().name.c_str(),
        //               timestamp_to_string(ppp.aos_time).c_str(),
        //               timestamp_to_string(ppp.los_time).c_str(),
        //               ppp.max_elevation);
    }

    void AutoTrackScheduler::setQTH(double qth_lon, double qth_lat, double qth_alt)
    {
        std::lock_guard<std::mutex> lock(upcoming_satellite_passes_mtx);
        this->qth_lon = qth_lon;
        this->qth_lat = qth_lat;
        this->qth_alt = qth_alt;
        // backend_needs_update = true;
    }

    void AutoTrackScheduler::setEngaged(bool v, double curr_time)
    {
        // Fork: a lock that is always let go. Working out the passes can throw, and that left it taken,
        // so autotrack then waited on it for ever
        std::lock_guard<std::mutex> lock(upcoming_satellite_passes_mtx);
        autotrack_engaged = v;
        updateAutotrackPasses(curr_time);

        if (autotrack_engaged && upcoming_satellite_passes_sel.size() > 0)
        {
            TrackedObject obj;
            for (auto &v : enabled_satellites)
                if (v.norad == upcoming_satellite_passes_sel[0].norad)
                    obj = v;
            // Fork: a throwing callback left the lock taken, and autotrack then waited on it for ever
            run_guarded("engage callback", upcoming_satellite_passes_sel[0].norad, [&]() { eng_callback(autotrack_cfg, upcoming_satellite_passes_sel[0], obj); });
            autotrack_pass_has_started = false;
        }
        else
        {
            autotrack_engaged = false;
        }
    }

    bool AutoTrackScheduler::getEngaged() { return autotrack_engaged; }

    std::vector<TrackedObject> AutoTrackScheduler::getTracked() { return enabled_satellites; }

    void AutoTrackScheduler::setTracked(std::vector<TrackedObject> tracked)
    {
        std::lock_guard<std::mutex> lock(upcoming_satellite_passes_mtx);
        enabled_satellites = tracked;
    }

    void AutoTrackScheduler::start()
    {
        backend_should_run = true;
        backend_thread = std::thread(&AutoTrackScheduler::backend_run, this);
    }

    void AutoTrackScheduler::backend_run()
    {
        while (backend_should_run)
        {
            // Fork: an error here used to leave the autotrack thread and end the program; it is
            // logged and autotrack tries again on the next round
            try
            {
                processAutotrack(getTime());
            }
            catch (const std::exception &e)
            {
                logger->error("Autotrack failed: %s", e.what());
            }
            catch (...)
            {
                logger->error("Autotrack failed with an unknown error");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    AutoTrackCfg AutoTrackScheduler::getAutoTrackCfg() { return autotrack_cfg; }

    void AutoTrackScheduler::setAutoTrackCfg(AutoTrackCfg v)
    {
        std::lock_guard<std::mutex> lock(upcoming_satellite_passes_mtx);
        autotrack_cfg = v;
    }
} // namespace satdump