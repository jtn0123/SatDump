#pragma once

#include "common/tracking/tle.h"
#include "core/exception.h"
#include "db/db_handler.h"
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace satdump
{
    struct KeplerData
    {
        int satellite_number;                 // Satellite number
        long element_number;                  // Element number
        std::string name;                     // Satellite Name
        std::string designator;               // International designator
        double epoch;                         // Epoch (Unix Time)
        double inclination;                   // Inclination
        double right_ascension;               // Right Ascension of the Ascending Node [Degrees]
        double eccentricity;                  // Eccentricity
        double argument_of_perigee;           // Argument of Perigee [Degrees]
        double mean_anomaly;                  // Mean Anomaly [Degrees]
        double mean_motion;                   // Mean Motion [Revs per day]
        double derivative_mean_motion;        // First Time Derivative of the Mean Motion divided by two
        double second_derivative_mean_motion; // Second Time Derivative of Mean Motion divided by six
        double bstar_drag_term;               // BSTAR drag term
        int revolutions_at_epoch;             // Number of revolutions around Earth at epoch
    };

    // Fork: the TLE lines for these elements, each value rounded into its columns of the TLE
    // layout, with both checksums. Throws when a TLE cannot hold them (see keplerTleProblem)
    TLE keplerToTle(KeplerData kep);

    // Fork: why keplerToTle cannot write these elements as a TLE, or "" when it can. It writes the
    // lines to find out, so this refuses exactly what cannot be written: a value that is not a
    // number, an epoch outside the years 2000 to 2099 once rounded, a drag term (BSTAR) or second
    // derivative of the mean motion of 0.999995 or more in size (1 or more once rounded to five
    // digits), a catalogue, element set or revolution number below 0, and a value too wide for
    // its columns once rounded: a first derivative of 0.999999995 or more in size, an
    // eccentricity of 0.99999995 or more or -0.00000005 or less, an angle of 999.99995 or more or
    // -99.99995 or less, a mean motion of 99.999999995 or more or -9.999999995 or less. Element
    // files with such an entry are refused when they are read, and keplerToTle throws for them
    std::string keplerTleProblem(const KeplerData &kep);

    // Fork: the 8 columns a TLE gives the drag term (BSTAR) and the second derivative of the mean
    // motion: a sign (' ' or '-'), five digits with a decimal point in front of them, then the
    // exponent, '-' and one digit: " 12345-4" is 0.12345e-4. The value is rounded to five
    // significant digits (halves away from zero, as the decimal it was given as) and 0 is
    // " 00000-0". Below 1e-10 the exponent stays at 9 and digits are lost, down to " 00001-9"
    // (1e-14); values below 5e-15 round to 0. None when the value is not a finite number or
    // rounds to 1 or more in size (0.999995 or more), which a TLE cannot hold
    std::optional<std::string> strLE(double val);
    bool ccsdsOmmToKepler(std::string omm_line, KeplerData &kep_out);
    bool tleToKepler(TLE tle, KeplerData &kep_out);

    std::vector<KeplerData> parseCcsdsOmmFile(std::string fileCont);

    // Fork: reads orbit elements in either text format: CCSDS OMM CSV (one satellite per line,
    // as the default sources send) or TLE text (a name line, then lines 1 and 2, per satellite).
    // bad_entries is set to the number of entries that could not be read.
    std::vector<KeplerData> parseKeplerText(const std::string &text, int &bad_entries);

    // Fork: a file:// URL is read from disk once (see readKeplerFile); other URLs are downloaded.
    // Entries of a download that cannot be read are skipped, with one warning that counts them
    std::vector<KeplerData> tryFetchOMMFileFromURL(std::string url_str);

    // Fork: reads the element file a file:// URL names. A file that is missing or cannot be read,
    // holds no elements, or has any entry that cannot be read, gives one warning and no elements,
    // so the elements already in the database are kept as they are.
    std::vector<KeplerData> readKeplerFile(const std::string &url_str);
    // Fork: as tryFetchOMMFileFromURL, entries that cannot be read are skipped with a warning
    std::vector<KeplerData> tryFetchSingleOMMwithNorad(int norad);

    class KeplerDBHandler : public DBHandlerBase
    {
    private:
        struct AutoUpdateKeplersEvent
        {
        };

    public:
        KeplerDBHandler(std::shared_ptr<DBHandler> h);

        void init();

        void putKepler(KeplerData kep);
        bool getKepler(KeplerData &kep, int norad, time_t time = -1);
        std::vector<KeplerData> getAllNewestKepler();

        void autoUpdateKeplers();
        void updateKeplerDatabase();

        // Fork: whether the settings name at least one element source and every one is a local
        // file (file:// URLs, no single satellites to download), as the Controller sets them
        static bool allSourcesAreLocal();

    public: // Legacy
        std::optional<TLE> get_from_norad(int norad)
        {
            KeplerData kep;
            if (getKepler(kep, norad))
                return storedToTle(kep);
            else
                return {};
        }

        std::optional<TLE> get_from_norad_time(int norad, time_t timestamp)
        {
            // TODOTLE Space-track

            KeplerData kep;
            if (getKepler(kep, norad, timestamp))
                return storedToTle(kep);
            else
                return {};
        }

    private:
        // Fork: the TLE for elements read from the database, or none, with a warning, when they
        // cannot be written as one (stored before such elements were refused). keplerToTle threw,
        // which stopped SatDump at start-up
        std::optional<TLE> storedToTle(const KeplerData &kep);

        std::vector<TLE> get_all_tles()
        {
            std::vector<TLE> tles;
            auto keps = getAllNewestKepler();
            for (auto &kep : keps)
                if (std::optional<TLE> tle = storedToTle(kep)) // Fork: see storedToTle
                    tles.push_back(*tle);
            return tles;
        }

    public:
        std::vector<TLE> all_;
    };
} // namespace satdump