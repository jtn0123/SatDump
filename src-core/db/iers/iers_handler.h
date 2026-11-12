#pragma once

#include "common/tracking/tle.h"
#include "core/exception.h"
#include "db/db_handler.h"

namespace satdump
{
    struct IERSInfo
    {
        time_t time;
        double polar_dx;
        double polar_dy;
        double ut1_utc;
        int leap_seconds;
    };

    class IersDBHandler : public DBHandlerBase
    {
    private:
        struct AutoUpdateIersEvent
        {
        };

    public:
        IersDBHandler(std::shared_ptr<DBHandler> h);

        void init();
        void updateIERS();

        // Stores the Bulletin A entries of the IERS JSON (finals2000A) in the iers table and
        // logs a summary of the entries that could not be read. False when the text is not
        // IERS JSON at all. Fork: each entry stands alone (stored by its date, replacing the
        // one stored for that date): those that can be read are stored, those that cannot are
        // skipped with one warning. False, with nothing stored, also when the JSON has no
        // EOP.data.timeSeries list or no entry in it can be used.
        bool storeBulletinA(const std::string &res);

        // Fork: stores the leap second list (leap-seconds.list: NTP time, TAI-UTC and a comment
        // on each line). False, with one warning and nothing stored, when any line cannot be
        // read, there are no entries, the times or the TAI-UTC values do not go up from each
        // entry to the next, or the list has no expiry line ("#@") or does not match its hash
        // line ("#h", the SHA-1 both published layouts end with).
        bool storeLeapSeconds(const std::string &text);

        // Fork: where the IERS bulletin and the leap second list come from: the settings
        // "iers_bulletin_url" and "leap_seconds_url" of satdump_general. Either can be a local
        // file (file://). "" unless the setting is {"value": "<text>"}: that file is then not
        // updated. The code has no address of its own, but the settings are settings.json merged
        // over the installed satdump_cfg.json, which holds the web addresses SatDump always used
        // (see iersUrlSetting in iers_handler.cpp).
        static std::string bulletinUrl();
        static std::string leapSecondsUrl();

        IERSInfo getBestIERSInfo(double time);
    };
} // namespace satdump