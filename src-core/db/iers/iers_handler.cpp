#include "iers_handler.h"
#include "core/config.h"
#include "core/plugin.h"
#include "db/db_handler.h"
#include "logger.h"
#include "nlohmann/json.hpp"
#include "utils/file_url.h"
#include "utils/http.h"
#include "utils/sha1.h"
#include <array>
#include <cstdint>
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

    // Fork: a text setting of satdump_general ({"value": "<address>"}), or "" when it is in any
    // other form or empty, and then nothing is fetched. The code has no address of its own. But
    // settings.json is merged over the installed satdump_cfg.json, whose values are the web
    // addresses SatDump always used. So an address is used only when settings.json gives it as
    // {"value": "<text>"}, or leaves the setting out (or gives an object without "value"), which
    // means that web address. Anything else is no address: {"value": ""}, {"value": null} (the
    // merge removes "value"), a value that is not text, null (the merge removes the setting) or
    // a setting that is not an object
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

    // Fork: the digits of a line of the leap second list up to its comment, as its hash is made of
    // the digits of the "#$" and "#@" times and of the entries, in the order of the file
    static void addLeapHashDigits(std::string &to, const std::string &text)
    {
        for (char ch : text)
        {
            if (ch == '#')
                break;
            if (ch >= '0' && ch <= '9')
                to += ch;
        }
    }

    // Fork: the five hexadecimal words after "#h" (as in "a9bad145 84c31c70 758402aa b37bfd54
    // 5923836a"). A word with leading zeros may be written without them, so each is read as a
    // number. False unless there are five words of 1 to 8 hexadecimal digits
    static bool readLeapHashLine(const std::string &words, std::array<uint32_t, 5> &hash)
    {
        std::istringstream fields(words);
        std::string w;
        size_t n = 0;
        while (fields >> w)
        {
            if (n == hash.size() || w.size() > 8 || w.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
                return false;
            hash[n++] = (uint32_t)std::stoul(w, nullptr, 16);
        }
        return n == hash.size();
    }

    // Fork: replaces the parser that split lines on single spaces, so the tab-separated list
    // published by IERS stored nothing. The whole list is refused, with one warning, unless the
    // times and the TAI-UTC values both go up from each entry to the next, it has its expiry line
    // ("#@" and a time), and it matches its hash line ("#h", a SHA-1 of the times and entries),
    // as both published layouts do. A list cut short inside its last value ("3692217600<tab>3")
    // was stored as 3 s from 2017 on; one cut short between two lines (before "3692217600 37")
    // has no hash line, as that is the last line, and gave 36 s from 2017 on
    bool IersDBHandler::storeLeapSeconds(const std::string &text)
    {
        std::vector<std::pair<long long, int>> entries;
        std::istringstream ls_stream(text);
        std::string l;
        int line_number = 0;
        bool has_expiry = false, has_hash = false;
        std::string hashed;
        std::array<uint32_t, 5> hash = {};
        while (std::getline(ls_stream, l))
        {
            line_number++;
            size_t first = l.find_first_not_of(" \t\r");
            if (first == std::string::npos)
                continue;
            if (l[first] == '#')
            {
                char kind = first + 1 < l.size() ? l[first + 1] : ' ';
                if (kind == '@')
                {
                    std::istringstream expiry_fields(l.substr(first + 2));
                    long long expiry = 0;
                    if (expiry_fields >> expiry)
                        has_expiry = true;
                }
                if (kind == '@' || kind == '$') // the expiry time, and the time of the last update
                    addLeapHashDigits(hashed, l.substr(first + 2));
                if (kind == 'h')
                    has_hash = readLeapHashLine(l.substr(first + 2), hash);
                continue;
            }

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
            if (!entries.empty() && (ntp_time - 2208988800LL <= entries.back().first || leaps <= entries.back().second))
            {
                logger->warn("The leap second list is damaged: line %d (%s) does not have a later time and more leap seconds than the entry before; "
                             "the leap seconds already loaded are kept",
                             line_number, l.c_str());
                return false;
            }
            entries.push_back({ntp_time - 2208988800LL, leaps});
            addLeapHashDigits(hashed, l);
        }

        if (entries.empty())
        {
            logger->warn("The leap second list has no entries; the leap seconds already loaded are kept");
            return false;
        }
        if (!has_expiry)
        {
            logger->warn("The leap second list has no expiry line (#@ and a time), which both published layouts have; the leap seconds already loaded are kept");
            return false;
        }
        if (!has_hash)
        {
            logger->warn("The leap second list has no hash line (#h and five hexadecimal words, the last line of a published list), so it may be cut short; "
                         "the leap seconds already loaded are kept");
            return false;
        }
        if (sha1Digest(hashed) != hash)
        {
            logger->warn("The leap second list does not match its hash line (#h), so it was cut short or changed; the leap seconds already loaded are kept");
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