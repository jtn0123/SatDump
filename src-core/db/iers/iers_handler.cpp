#include "iers_handler.h"
#include "core/config.h"
#include "core/plugin.h"
#include "db/db_handler.h"
#include "logger.h"
#include "nlohmann/json.hpp"
#include "utils/http.h"
#include <exception>
#include <sstream>

namespace satdump
{
    IersDBHandler::IersDBHandler(std::shared_ptr<DBHandler> h) : DBHandlerBase(h) {}

    void IersDBHandler::init()
    {
        // Create IERS Table
        std::string sql_create_iers = "CREATE TABLE IF NOT EXISTS iers("
                                      "time INT PRIMARY KEY NOT NULL,"
                                      "a_pole_x FLOAT NOT NULL,"
                                      "a_pole_y FLOAT NOT NULL,"
                                      "ut1_utc FLOAT NOT NULL);";
        if (h->run_sql(sql_create_iers))
            throw satdump_exception("Failed creating IERS database!");

        // Create Leap Seconds Table
        std::string sql_create_leap_seconds = "CREATE TABLE IF NOT EXISTS leap_seconds("
                                              "time INT PRIMARY KEY NOT NULL,"
                                              "leap_seconds INT NOT NULL);";
        if (h->run_sql(sql_create_leap_seconds))
            throw satdump_exception("Failed creating leap seconds database!");

        time_t last_update = std::stoull(h->get_meta("iers_last_updated", "0"));
        bool honor_setting = true; // Fork: the "iers_auto_update" setting can switch the downloads off
        try
        {
            honor_setting = satdump_cfg.getValueFromSatDumpGeneral<bool>("iers_auto_update");
        }
        catch (std::exception &e)
        {
            logger->warn("Could not read the IERS auto-update setting (%s), keeping it on", e.what());
        }
        time_t update_interval = 3600 * 60; // TODOREWORK

#if 0
        // Update now, if needed
        time_t now = time(NULL);
        if (/*(honor_setting && now > last_update + update_interval) ||*/ h->get_table_size("iers") <= 0)
        {
            updateIERS();
            last_update = now;
        }
#endif

        // Schedule updates while running
        if (honor_setting)
        {
            eventBus->register_handler<AutoUpdateIersEvent>([this](AutoUpdateIersEvent) { updateIERS(); });
            std::shared_ptr<AutoUpdateIersEvent> evt = std::make_shared<AutoUpdateIersEvent>();
            taskScheduler->add_task<AutoUpdateIersEvent>("auto_iers_update", evt, last_update, update_interval);
        }
    }

    void IersDBHandler::updateIERS()
    {
        std::string url_str = "https://datacenter.iers.org/products/eop/rapid/standard/json/finals2000A.all.json"; // TODOREWORK

        // IERS Bulletin A/B
        logger->info("Downloading IERS Bulletins from " + url_str + "...");
        std::string res;
        if (perform_http_request(url_str, res))
        {
            logger->error("Error fetching IERS Bulletins!");
            return;
        }

        if (!storeBulletinA(res))
            return;

        // Leap seconds (bulletin C)
        url_str = "https://hpiers.obspm.fr/iers/bul/bulc/ntp/leap-seconds.list";
        logger->info("Downloading Leap Seconds Bulletins from " + url_str + "...");
        if (perform_http_request(url_str, res))
        {
            logger->error("Error fetching Leap Seconds Bulletins!");
            return;
        }

        try
        {
            std::istringstream ls_stream(res);
            std::string l;
            while (std::getline(ls_stream, l))
            {
                if (l.size() > 0 && l[0] == '#')
                    continue;

                std::vector<std::string> parts;
                std::string p;
                char lastc = ' ';
                for (auto &c : l)
                {
                    if (c == ' ' && lastc != ' ')
                    {
                        parts.push_back(p);
                        p.clear();
                    }

                    p.push_back(c);
                    lastc = c;
                }
                parts.push_back(p);

                if (parts.size() == 6)
                {
                    uint64_t time = std::stoull(parts[0]) - 2208988800;
                    int leaps = std::stod(parts[1]);

                    std::string sql = "INSERT INTO leap_seconds (time, leap_seconds) VALUES (" + std::to_string(time) + ", '" + std::to_string(leaps) +
                                      "') ON CONFLICT(time) DO UPDATE SET leap_seconds='" + std::to_string(leaps) + "';";

                    char *err = NULL;
                    if (sqlite3_exec(h->db, sql.c_str(), NULL, 0, &err))
                    {
                        logger->error(err);
                        sqlite3_free(err);
                    }
                }
            }
        }
        catch (std::exception &e)
        {
            logger->error("Leap second parsing error! %s", e.what());
            return;
        }

        // Update last update timestamp & other stuff
        h->set_meta("iers_last_updated", std::to_string(time(0)));
        logger->info("%d IERS Records in database!", h->get_table_size("iers"));
    }

    // Fork: whether an IERS field is missing or blank, as in the rows for dates that have no
    // prediction yet
    static bool iers_field_blank(const nlohmann::json &f)
    {
        if (f.is_null())
            return true;
        if (!f.is_string())
            return false;
        return f.get_ref<const std::string &>().find_first_not_of(" \t") == std::string::npos;
    }

    // Fork: split out of updateIERS() so it can be tested without a download
    bool IersDBHandler::storeBulletinA(const std::string &res)
    {
        try
        {
            nlohmann::json j = nlohmann::json::parse(res);

            h->tr_begin();

            // Fork: entries that cannot be read were skipped silently. Rows with no values yet
            // (dates not predicted) are still skipped, with a count at debug level. Entries
            // whose values cannot be read are skipped too, with one warning that counts them
            // and names the first.
            int entries = 0, not_predicted = 0, skipped = 0;
            std::string first_skipped;

            for (auto &v : j["EOP"]["data"]["timeSeries"])
            {
                entries++;
                try
                {
                    if (v["dataEOP"]["pole"][0]["source"] == "BulletinA")
                    {
                        if (iers_field_blank(v["dataEOP"]["pole"][0]["X"]) && iers_field_blank(v["dataEOP"]["pole"][0]["Y"]) && iers_field_blank(v["dataEOP"]["UT"][0]["UT1-UTC"]))
                        {
                            not_predicted++;
                            continue;
                        }

                        time_t time = ((std::stod(v["time"]["MJD"].get<std::string>()) - 40587) * 86400);
                        double a_pole_x = std::stod(v["dataEOP"]["pole"][0]["X"].get<std::string>());
                        double a_pole_y = std::stod(v["dataEOP"]["pole"][0]["Y"].get<std::string>());
                        double ut1_utc = std::stod(v["dataEOP"]["UT"][0]["UT1-UTC"].get<std::string>());

                        std::string sql = "INSERT INTO iers (time, a_pole_x, a_pole_y, ut1_utc) VALUES (" + std::to_string(time) + ", '" + std::to_string(a_pole_x) + "', '" +
                                          std::to_string(a_pole_y) + "', '" + std::to_string(ut1_utc) + "') ON CONFLICT(time) DO UPDATE SET a_pole_x='" + std::to_string(a_pole_x) + "', a_pole_y='" +
                                          std::to_string(a_pole_y) + "', ut1_utc='" + std::to_string(ut1_utc) + "';";

                        char *err = NULL;
                        if (sqlite3_exec(h->db, sql.c_str(), NULL, 0, &err))
                        {
                            logger->error(err);
                            sqlite3_free(err);
                        }
                    }
                    else
                    {
                        logger->error("First bulletin should be A!");
                    }
                }
                catch (std::exception &e)
                {
                    if (skipped++ == 0)
                    {
                        const nlohmann::json &mjd = v["time"]["MJD"];
                        first_skipped = "MJD " + (mjd.is_string() ? mjd.get<std::string>() : mjd.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace)) + ": " + e.what();
                    }
                }
            }

            h->tr_end();

            if (not_predicted > 0)
                logger->debug("IERS: %d of %d entries have no values yet (dates not predicted), skipped", not_predicted, entries);
            if (skipped > 0)
                logger->warn("IERS: %d of %d entries could not be read and were skipped (the first, %s)", skipped, entries, first_skipped.c_str());
        }
        catch (std::exception &e)
        {
            logger->error("Error parsing IERS data! %s", e.what());
            return false;
        }

        return true;
    }

    IERSInfo IersDBHandler::getBestIERSInfo(double time)
    {
        IERSInfo r;
        sqlite3_stmt *res;

        if (sqlite3_prepare_v2(h->db, ("select a_pole_x,a_pole_y,ut1_utc from iers order by abs(" + std::to_string(time) + ") asc limit 1").c_str(), -1, &res, 0))
            logger->error("Couldn't fetch IERS data from DB! " + std::string(sqlite3_errmsg(h->db)));
        else if (sqlite3_step(res) == SQLITE_ROW)
        {
            r.polar_dx = sqlite3_column_double(res, 0) * 1000; // todorework to marcsec
            r.polar_dy = sqlite3_column_double(res, 1) * 1000; // todorework to marcsec
            r.ut1_utc = sqlite3_column_double(res, 2);
        }

        sqlite3_finalize(res);

        if (sqlite3_prepare_v2(h->db, ("select leap_seconds from leap_seconds where " + std::to_string(time) + " >= time order by time desc limit 1").c_str(), -1, &res, 0))
            logger->error("Couldn't fetch Leap Seconds data from DB! " + std::string(sqlite3_errmsg(h->db)));
        else if (sqlite3_step(res) == SQLITE_ROW)
        {
            r.leap_seconds = sqlite3_column_int(res, 0);
        }

        sqlite3_finalize(res);

        return r;
    }
} // namespace satdump