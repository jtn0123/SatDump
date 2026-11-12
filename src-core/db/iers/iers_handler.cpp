#include "iers_handler.h"
#include "core/config.h"
#include "core/plugin.h"
#include "db/db_handler.h"
#include "logger.h"
#include "nlohmann/json.hpp"
#include "utils/file_url.h"
#include "utils/http.h"
#include <exception>
#include <sstream>
#include <utility>
#include <vector>

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
        // Fork: the "iers_auto_update" setting can switch the updates off. Off when it cannot be
        // read (it was on), as the fork's defaults never download anything
        bool honor_setting = false;
        try
        {
            honor_setting = satdump_cfg.getValueFromSatDumpGeneral<bool>("iers_auto_update");
        }
        catch (std::exception &e)
        {
            logger->warn("Could not read the IERS auto-update setting (%s), keeping it off", e.what());
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

            // Fork: when nothing would be downloaded (each address is a local file, file://, or is
            // not set), the update runs now, before start-up goes on (as the orbit elements do, see
            // KeplerDBHandler::autoUpdateKeplers). Downloads still run on the task thread
            std::string bulletin = bulletinUrl(), leap_seconds = leapSecondsUrl();
            if ((bulletin.empty() || isFileUrl(bulletin)) && (leap_seconds.empty() || isFileUrl(leap_seconds)))
            {
                // Fork: as for the orbit elements, an error here must not leave start-up
                try
                {
                    updateIERS();
                }
                catch (const std::exception &e)
                {
                    logger->error("Reading the IERS files at start-up failed (%s); the data in the database is used", e.what());
                }
                last_update = time(0);
            }

            std::shared_ptr<AutoUpdateIersEvent> evt = std::make_shared<AutoUpdateIersEvent>();
            taskScheduler->add_task<AutoUpdateIersEvent>("auto_iers_update", evt, last_update, update_interval);
        }
    }

    // Fork: a text setting of satdump_general ({"value": "<address>"}), or "" when it is missing,
    // empty or not in that form. There is no built-in address to fall back on: a setting that
    // cannot be read must never lead to a download (the addresses SatDump always used are the
    // defaults in satdump_cfg.json)
    static std::string iersUrlSetting(const std::string &name)
    {
        const nlohmann::ordered_json &cfg = satdump_cfg.main_cfg;
        if (cfg.contains("satdump_general") && cfg.at("satdump_general").is_object() && cfg.at("satdump_general").contains(name))
        {
            const nlohmann::ordered_json &v = cfg.at("satdump_general").at(name);
            if (v.is_object() && v.contains("value") && v.at("value").is_string())
                return v.at("value").get<std::string>();
        }
        return "";
    }

    std::string IersDBHandler::bulletinUrl() { return iersUrlSetting("iers_bulletin_url"); }
    std::string IersDBHandler::leapSecondsUrl() { return iersUrlSetting("leap_seconds_url"); }

    // Fork: reads a local file (file://) once, with one warning if it cannot be read, or downloads
    // anything else. An address that is not set is one warning and nothing is read. `out` is
    // cleared first (the second download used to be added to the first)
    static bool fetchIersText(const std::string &url, const char *what, const char *setting, std::string &out)
    {
        out.clear();
        if (url.empty())
        {
            logger->warn("No address is set for the %s (the setting %s is missing, empty or not {\"value\": \"<address>\"}), so it is not updated", what, setting);
            return false;
        }
        if (isFileUrl(url))
        {
            std::string error;
            if (readFileUrl(url, out, error))
            {
                logger->info("Reading the %s from %s", what, url.c_str());
                return true;
            }
            logger->warn("Cannot read the %s file %s (%s); the data already loaded is kept", what, url.c_str(), error.c_str());
            return false;
        }

        logger->info("Downloading the %s from %s...", what, url.c_str());
        if (perform_http_request(url, out))
        {
            logger->error("Error fetching the %s!", what);
            return false;
        }
        return true;
    }

    void IersDBHandler::updateIERS()
    {
        // Fork: the bulletin and the leap seconds are each stored when they can be read; a missing
        // leap second file no longer stops the bulletin from being stored, nor the other way round.
        // The time of the last update is set only when both were stored, as before.
        std::string res;
        bool bulletin_ok = fetchIersText(bulletinUrl(), "IERS bulletin", "iers_bulletin_url", res) && storeBulletinA(res);
        bool leap_ok = fetchIersText(leapSecondsUrl(), "leap second list", "leap_seconds_url", res) && storeLeapSeconds(res);

        // Update last update timestamp & other stuff
        if (bulletin_ok && leap_ok)
            h->set_meta("iers_last_updated", std::to_string(time(0)));
        logger->info("%d IERS Records in database!", h->get_table_size("iers"));
    }

    // Fork: replaces the parser that split lines on single spaces, so the tab-separated list
    // published by IERS stored nothing
    bool IersDBHandler::storeLeapSeconds(const std::string &text)
    {
        std::vector<std::pair<long long, int>> entries;
        std::istringstream ls_stream(text);
        std::string l;
        int line_number = 0;
        while (std::getline(ls_stream, l))
        {
            line_number++;
            size_t first = l.find_first_not_of(" \t\r");
            if (first == std::string::npos || l[first] == '#')
                continue;

            // NTP time (seconds since 1900), TAI-UTC, then nothing or a comment
            std::istringstream fields(l);
            long long ntp_time = 0;
            int leaps = 0;
            std::string rest;
            bool ok = static_cast<bool>(fields >> ntp_time >> leaps) && ntp_time >= 2208988800LL;
            if (ok && fields >> rest)
                ok = rest[0] == '#';
            if (!ok)
            {
                logger->warn("The leap second list cannot be read (line %d: %s); the leap seconds already loaded are kept", line_number, l.c_str());
                return false;
            }
            entries.push_back({ntp_time - 2208988800LL, leaps});
        }

        if (entries.empty())
        {
            logger->warn("The leap second list has no entries; the leap seconds already loaded are kept");
            return false;
        }

        h->tr_begin();
        for (auto &e : entries)
        {
            std::string sql = "INSERT INTO leap_seconds (time, leap_seconds) VALUES (" + std::to_string(e.first) + ", '" + std::to_string(e.second) +
                              "') ON CONFLICT(time) DO UPDATE SET leap_seconds='" + std::to_string(e.second) + "';";

            char *err = NULL;
            if (sqlite3_exec(h->db, sql.c_str(), NULL, 0, &err))
            {
                logger->error(err);
                sqlite3_free(err);
            }
        }
        h->tr_end();
        return true;
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

            // Fork: JSON that is not shaped like a bulletin (an array, "EOP" holding text, ...) is
            // refused before the database transaction starts. It threw inside the transaction,
            // which was then left open
            nlohmann::json *series = nullptr;
            if (j.is_object() && j.contains("EOP") && j["EOP"].is_object() && j["EOP"].contains("data") && j["EOP"]["data"].is_object() &&
                j["EOP"]["data"].contains("timeSeries") && j["EOP"]["data"]["timeSeries"].is_array())
                series = &j["EOP"]["data"]["timeSeries"];
            if (series == nullptr)
            {
                logger->error("Error parsing IERS data! The JSON has no EOP.data.timeSeries list; the data already loaded is kept");
                return false;
            }

            h->tr_begin();

            // Fork: entries that cannot be read were skipped silently. Rows with no values yet
            // (dates not predicted) are still skipped, with a count at debug level. Entries
            // whose values cannot be read are skipped too, with one warning that counts them
            // and names the first.
            int entries = 0, not_predicted = 0, skipped = 0, stored = 0;
            std::string first_skipped;

            for (auto &v : *series)
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
                        else
                            stored++;
                    }
                    else
                    {
                        logger->error("First bulletin should be A!");
                    }
                }
                catch (std::exception &e)
                {
                    // Fork: an entry that is not an object (a number, text) no longer throws again here,
                    // inside the transaction
                    if (skipped++ == 0)
                    {
                        std::string mjd_text = "?";
                        if (v.is_object() && v.contains("time") && v["time"].is_object() && v["time"].contains("MJD"))
                        {
                            const nlohmann::json &mjd = v["time"]["MJD"];
                            mjd_text = mjd.is_string() ? mjd.get<std::string>() : mjd.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
                        }
                        first_skipped = "MJD " + mjd_text + ": " + e.what();
                    }
                }
            }

            h->tr_end();

            if (not_predicted > 0)
                logger->debug("IERS: %d of %d entries have no values yet (dates not predicted), skipped", not_predicted, entries);
            if (skipped > 0)
                logger->warn("IERS: %d of %d entries could not be read and were skipped (the first, %s)", skipped, entries, first_skipped.c_str());

            // Fork: JSON with no usable entry (an empty or unrelated file) is not an update
            if (stored == 0)
            {
                logger->warn("IERS: the bulletin has no entries that can be used; the data already loaded is kept");
                return false;
            }
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
        // Fork: all zero when there is no data (the fields were left unset), and the entry nearest
        // to `time` is used: the query sorted by a constant (abs(time) alone), so it returned
        // whichever entry the database listed first
        IERSInfo r = {};
        r.time = time;
        sqlite3_stmt *res;

        if (sqlite3_prepare_v2(h->db, ("select a_pole_x,a_pole_y,ut1_utc from iers order by abs(time - " + std::to_string(time) + ") asc limit 1").c_str(), -1, &res, 0))
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