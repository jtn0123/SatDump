// Fork: a database held in memory (or in a file the test names), with the orbit element (Kepler)
// and IERS tables, made the current one (satdump::db_keplers) under settings of the test's own.
// The settings and the database in use before are put back when the rig goes. Nothing is read
// from the network.
#pragma once

#include "core/config.h"
#include "core/plugin.h"
#include "db/db_handler.h"
#include "db/iers/iers_handler.h"
#include "db/kepler/kepler_handler.h"
#include "init.h"
#include "nlohmann/json.hpp"
#include <memory>
#include <string>
#include <vector>

namespace test_rig
{
    // The "satdump_general" settings for the updates, as satdump_cfg.json holds them. Empty
    // addresses are left out of the settings.
    inline nlohmann::ordered_json general(const std::string &kepler_interval, bool iers_update, const std::string &bulletin_url = "", const std::string &leap_url = "")
    {
        nlohmann::ordered_json g;
        g["kepler_update_interval"]["value"] = kepler_interval;
        g["iers_auto_update"]["value"] = iers_update;
        if (bulletin_url.size() > 0)
            g["iers_bulletin_url"]["value"] = bulletin_url;
        if (leap_url.size() > 0)
            g["leap_seconds_url"]["value"] = leap_url;
        return g;
    }

    // The "kepler_settings" part: element sources and single satellites to download
    inline nlohmann::ordered_json kepler_sources(const std::vector<std::string> &urls, const std::vector<int> &norads = {})
    {
        nlohmann::ordered_json k;
        k["urls_to_fetch"] = urls;
        k["norads_to_fetch"] = norads;
        return k;
    }

    struct OfflineRig
    {
        nlohmann::ordered_json old_cfg;
        std::shared_ptr<satdump::EventBus> old_bus;
        std::shared_ptr<satdump::KeplerDBHandler> old_keplers;
        std::shared_ptr<satdump::DBHandler> db;
        std::shared_ptr<satdump::KeplerDBHandler> keplers;
        std::shared_ptr<satdump::IersDBHandler> iers;

        // Sets the settings, then makes the database (init() runs the start-up updates). db_path
        // names a database file to use instead of one held in memory
        OfflineRig(const nlohmann::ordered_json &general_settings, const nlohmann::ordered_json &kepler_settings = kepler_sources({}), const std::string &db_path = ":memory:")
        {
            // An event bus of its own: an update tells everything listening on the bus, and
            // objects of earlier tests (trackers, schedulers) stay listening after they are gone
            old_bus = satdump::eventBus;
            satdump::eventBus = std::make_shared<satdump::EventBus>();
            old_cfg = satdump::satdump_cfg.main_cfg;
            old_keplers = satdump::db_keplers;
            satdump::satdump_cfg.main_cfg["satdump_general"] = general_settings;
            satdump::satdump_cfg.main_cfg["kepler_settings"] = kepler_settings;

            db = std::make_shared<satdump::DBHandler>(db_path);
            db->subhandlers.push_back(keplers = std::make_shared<satdump::KeplerDBHandler>(db));
            db->subhandlers.push_back(iers = std::make_shared<satdump::IersDBHandler>(db));
            try
            {
                db->init();
            }
            catch (...) // a rig that throws here is never destroyed: what it changed is put back now
            {
                put_back();
                throw;
            }
            satdump::db_keplers = keplers;
        }

        ~OfflineRig() { put_back(); }

        OfflineRig(const OfflineRig &) = delete;
        OfflineRig &operator=(const OfflineRig &) = delete;

    private:
        void put_back()
        {
            // The updates init() scheduled must never run on a database that is gone (the task
            // thread is not started in the unit tests, this is to be safe)
            satdump::taskScheduler->del_task("auto_kepler_update_todorework");
            satdump::taskScheduler->del_task("auto_iers_update");
            // The database holds its tables and each table holds the database: the loop is
            // broken here, or none of them is ever freed
            db->subhandlers.clear();
            satdump::db_keplers = old_keplers;
            satdump::satdump_cfg.main_cfg = old_cfg;
            satdump::eventBus = old_bus;
        }
    };
} // namespace test_rig
