#include "core/config.h"
#include "kepler_handler.h"
#include "libs/predict/predict.h"
#include "logger.h"
#include "utils/file_url.h"
#include "utils/format.h"
#include "utils/http.h"
#include "utils/string.h"
#include <cctype>
#include <climits>
#include <cmath>
#include <cstring>
#include <exception>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>
#include "common/utils.h"

namespace satdump
{
    template <typename T>
    std::string strL(T val, int sz, bool leading_dec = false)
    {
        std::string str = to_string_with_precision(val, 30);

        if (str.size() > 2 && str[0] == '0' && str[1] == '.')
            str = leading_dec ? str.substr(2, str.size() - 2) : str.substr(1, str.size() - 1);

        if (str.size() > 3 && str[0] == '-' && str[1] == '0' && str[2] == '.')
            str = leading_dec ? ("-" + str.substr(3, str.size() - 3)) : ("-" + str.substr(2, str.size() - 2));

        if (str.size() > sz)
            str.resize(sz);
        if (str.size() < sz)
            while (str.size() < sz)
                str = " " + str;

        return str;
    }

    std::string strLE(double val, int sz)
    {
        if (val == 0)
        {
            std::string res(sz - 2, '0');
            return res + "-0";
        }
        else if (val < 1)
        {
            int mult = 0;
            while (fmod(val * pow(10, mult), 1) != 0)
                mult++;

            // logger->critical("%d %d", mult, mult - (sz - 3));

            int coef = mult - (sz - 3);

            int coefflen = std::to_string(coef).size();

            // Fork: a long long, not an int. The loop above ends by 10 x 2^53 at the latest, which
            // can be past an int: the cast was then undefined
            std::string res = std::to_string((long long)(val * pow(10, mult)));

            if (res.size() > sz - 1 - coefflen)
                res.resize(sz - 1 - coefflen);
            if (res.size() < sz - 1 - coefflen)
                while (res.size() < sz - 1 - coefflen)
                    res = " " + res;

            res += "-";
            res += std::to_string(coef);

            return res;
        }
        else
            throw satdump_exception("Error, value must be below 1!");
    }

    // Fork: see kepler_handler.h
    std::string keplerTleProblem(const KeplerData &kep)
    {
        const std::pair<const char *, double> values[] = {{"epoch", kep.epoch},
                                                          {"inclination", kep.inclination},
                                                          {"right ascension", kep.right_ascension},
                                                          {"eccentricity", kep.eccentricity},
                                                          {"argument of perigee", kep.argument_of_perigee},
                                                          {"mean anomaly", kep.mean_anomaly},
                                                          {"mean motion", kep.mean_motion},
                                                          {"first derivative of the mean motion", kep.derivative_mean_motion},
                                                          {"second derivative of the mean motion", kep.second_derivative_mean_motion},
                                                          {"drag term (BSTAR)", kep.bstar_drag_term}};
        for (auto &v : values)
            if (!std::isfinite(v.second))
                return std::string("the ") + v.first + " is not a finite number";

        // 2000-01-01 to 2100-01-01 UTC: the TLE gives the year in two digits, read back as 20yy
        if (kep.epoch < 946684800.0 || kep.epoch >= 4102444800.0)
            return "the epoch is not in the years 2000 to 2099";

        // A TLE holds these as five digits and a one-digit exponent, so 0 or a size from 1e-14 to
        // just under 1. strLE threw at 1 and above, and never finished below about 1e-292
        const std::pair<const char *, double> exponent_values[] = {{"second derivative of the mean motion", kep.second_derivative_mean_motion},
                                                                   {"drag term (BSTAR)", kep.bstar_drag_term}};
        for (auto &v : exponent_values)
            if (v.second != 0 && !(std::fabs(v.second) >= 1e-14 && std::fabs(v.second) < 1))
                return std::string("the ") + v.first + " is not 0 or of a size from 1e-14 to 1";

        return "";
    }

    // Fork: whether a designator has the OMM form, YYYY-NNN then 1 to 3 letters for the piece
    static bool isOmmDesignator(const std::string &d)
    {
        if (d.size() < 9 || d.size() > 11 || d[4] != '-')
            return false;
        for (size_t i = 0; i < d.size(); i++)
            if (i != 4 && !(i < 8 ? isdigit((unsigned char)d[i]) : isalpha((unsigned char)d[i])))
                return false;
        return true;
    }

    TLE keplerToTle(KeplerData kep)
    {
        // Fork: values a TLE cannot hold are an error, never a hang or a wrong line (see above)
        std::string problem = keplerTleProblem(kep);
        if (!problem.empty())
            throw satdump_exception("the elements of satellite " + std::to_string(kep.satellite_number) + " cannot be written as a TLE: " + problem);

        TLE tle;
        tle.name = kep.name;
        tle.norad = kep.satellite_number;
        tle.time = kep.epoch;

        int tle_id = kep.satellite_number > 99999 ? 99999 : kep.satellite_number;

        time_t tmptime = kep.epoch;
        tm timeS = *gmtime(&tmptime);
        int year = timeS.tm_year;
        // logger->critical(year);
        memset(&timeS, 0, sizeof(struct tm));
        timeS.tm_year = year;
        double yearT = timegm(&timeS);

        double days = (kep.epoch - yearT) / (3600.0 * 24.0);

        // Fork: an OMM designator ("2019-074AB") is written in TLE form ("19074AB "). Only the one
        // with a one-letter piece was, and longer ones were cut to "2019-074". Every designator is
        // padded with spaces to 8, as in a TLE; resize(8) padded the shorter ones TLE text gives
        // ("12049A") with NUL characters
        std::string des = kep.designator;
        if (isOmmDesignator(des))
            des = des.substr(2, 2) + des.substr(5, 3) + des.substr(8);
        des.resize(8, ' ');

        tle.line1 = "1 " +                                              // Line Number
                    strL(tle_id, 5) +                                   // Satellite Number
                    "U " +                                              // Classification
                    des +                                               // International Designator
                    " " + strL(year - 100, 2) +                         // EPoch Year (last 2)
                    strL(days, 12) +                                    // Epoch DOY
                    " " + strL(kep.derivative_mean_motion, 10) +        // First derivative mean motion
                    " " + strLE(kep.second_derivative_mean_motion, 8) + // Second derivative mean motion
                    " " + strLE(kep.bstar_drag_term, 8) +               // Drag term
                    " 0 " +                                             // Ephemeris type
                    strL(kep.element_number, 4) +                       // Element set number
                    "C";                                                // Checksum

        tle.line2 = "2 " +                                   // Line Number
                    strL(tle_id, 5) +                        // Satellite Number
                    " " + strL(kep.inclination, 8) +         // Inclination
                    " " + strL(kep.right_ascension, 8) +     // Right Ascension
                    " " + strL(kep.eccentricity, 7, 1) +     // Eccentricity
                    " " + strL(kep.argument_of_perigee, 8) + // Arg of perigee
                    " " + strL(kep.mean_anomaly, 8) +        // Mean anomaly
                    " " + strL(kep.mean_motion, 11) +        // Mean motion
                    strL(kep.revolutions_at_epoch, 5) +      // Revs at epoch
                    "C";                                     // Checksum

        return tle;
    }

    // Fork: a whole-number column of OMM CSV (catalogue number, element set, revolutions). It went
    // from std::stod straight into an int, which is undefined for "1e20" or "nan"
    static int ommWholeNumber(const std::string &text)
    {
        double v = std::stod(text);
        if (!(v >= 0 && v <= INT_MAX))
            throw std::out_of_range("not a whole number from 0 to INT_MAX");
        return (int)v;
    }

    bool ccsdsOmmToKepler(std::string omm, KeplerData &kep)
    {
        try
        {
            auto elems = splitString(omm, ',');

            if (elems.size() != 17)
                return false;

            std::tm timeS;
            double seconds;
            memset(&timeS, 0, sizeof(std::tm));
            if (sscanf(elems[2].c_str(), "%4d-%2d-%2dT%2d:%2d:%lf", &timeS.tm_year, &timeS.tm_mon, &timeS.tm_mday, &timeS.tm_hour, &timeS.tm_min, &seconds) == 6)
            {
                timeS.tm_year -= 1900;
                timeS.tm_mon -= 1;
                kep.epoch = timegm(&timeS) + seconds;
            }
            else // Fork: a line whose epoch cannot be read is not used (its epoch was left unset)
                return false;

            kep.name = elems[0];
            kep.designator = elems[1];

            kep.mean_motion = std::stod(elems[3]);
            kep.eccentricity = std::stod(elems[4]);
            kep.inclination = std::stod(elems[5]);
            kep.right_ascension = std::stod(elems[6]);
            kep.argument_of_perigee = std::stod(elems[7]);
            kep.mean_anomaly = std::stod(elems[8]);

            kep.satellite_number = ommWholeNumber(elems[11]);
            kep.element_number = ommWholeNumber(elems[12]);
            kep.revolutions_at_epoch = ommWholeNumber(elems[13]);

            kep.bstar_drag_term = std::stod(elems[14]);
            kep.derivative_mean_motion = std::stod(elems[15]);
            kep.second_derivative_mean_motion = std::stod(elems[16]);
        }
        catch (std::exception &e)
        {
            return false;
        }

        // Fork: a line with values a TLE cannot be made from cannot be used either. It was stored,
        // and making the TLE then threw, which stopped SatDump at start-up (exit 134)
        return keplerTleProblem(kep).empty();
    }

    bool tleToKepler(TLE tle, KeplerData &kep)
    {
        auto tle_ephems = predict_parse_tle(tle.line1.c_str(), tle.line2.c_str());

        if (!tle_ephems)
            return false;

        kep.satellite_number = tle_ephems->satellite_number; // Satellite number
        kep.element_number = tle_ephems->element_number;     // Element number
        kep.name = tle.name;                                 // Satellite Name
        // International designator. Fork: 8 characters at most; a full one ("99025BGQ") has no NUL
        // after it, and the copy read on into the bytes that follow
        kep.designator = std::string(tle_ephems->designator, strnlen(tle_ephems->designator, 8));

        std::tm timeS;
        memset(&timeS, 0, sizeof(std::tm));
        timeS.tm_year = (2000 + tle_ephems->epoch_year) - 1900;
        kep.epoch = timegm(&timeS) + tle_ephems->epoch_day * (3600. * 24.);

        kep.inclination = tle_ephems->inclination;                                     // Inclination
        kep.right_ascension = tle_ephems->right_ascension;                             // Right Ascension of the Ascending Node [Degrees]
        kep.eccentricity = tle_ephems->eccentricity;                                   // Eccentricity
        kep.argument_of_perigee = tle_ephems->argument_of_perigee;                     // Argument of Perigee [Degrees]
        kep.mean_anomaly = tle_ephems->mean_anomaly;                                   // Mean Anomaly [Degrees]
        kep.mean_motion = tle_ephems->mean_motion;                                     // Mean Motion [Revs per day]
        kep.derivative_mean_motion = tle_ephems->derivative_mean_motion;               // First Time Derivative of the Mean Motion divided by two
        kep.second_derivative_mean_motion = tle_ephems->second_derivative_mean_motion; // Second Time Derivative of Mean Motion divided by six
        kep.bstar_drag_term = tle_ephems->bstar_drag_term;                             // BSTAR drag term
        kep.revolutions_at_epoch = tle_ephems->revolutions_at_epoch;                   // Number of revolutions around Earth at epoch

        predict_destroy_orbital_elements(tle_ephems);

        // Fork: see ccsdsOmmToKepler (an exponent column that is not a digit can give a BSTAR of 1e16)
        return keplerTleProblem(kep).empty();
    }

    std::vector<KeplerData> parseCcsdsOmmFile(std::string fileCont)
    {
        std::istringstream omm_stream(fileCont);

        std::vector<KeplerData> keps;

        std::string this_line;
        while (std::getline(omm_stream, this_line))
        {
            KeplerData kep;
            if (ccsdsOmmToKepler(this_line, kep))
                keps.push_back(kep);
        }

        return keps;
    }

    // Fork: whether a TLE line (1 or 2, as `number` says) has the right length and checksum
    static bool tleLineValid(const std::string &l, char number)
    {
        if (l.size() != 69 || l[0] != number || l[1] != ' ' || !isdigit((unsigned char)l[68]))
            return false;
        int sum = 0;
        for (size_t i = 0; i < 68; i++)
        {
            if (l[i] == '-')
                sum++;
            else if (isdigit((unsigned char)l[i]))
                sum += l[i] - '0';
        }
        return l[68] - '0' == sum % 10;
    }

    // Fork: the lines of a text, without blank lines or the spaces (and \r) around each
    static std::vector<std::string> trimmedLines(const std::string &text)
    {
        std::vector<std::string> lines;
        std::istringstream stream(text);
        std::string line;
        while (std::getline(stream, line))
        {
            size_t first = line.find_first_not_of(" \t\r");
            if (first == std::string::npos)
                continue;
            lines.push_back(line.substr(first, line.find_last_not_of(" \t\r") - first + 1));
        }
        return lines;
    }

    // Fork: whether a line that stands where a satellite's name goes cannot be a name: a TLE line 2
    // (its name and line 1 lost) or the header line of OMM CSV
    static bool notASatelliteName(const std::string &l)
    {
        bool line2 = l.size() > 2 && l[0] == '2' && l[1] == ' ' && (isdigit((unsigned char)l[2]) || l[2] == ' ');
        return line2 || l.rfind("OBJECT_NAME,", 0) == 0;
    }

    // Fork: TLE text, a name line (optional, "0 " in front is dropped) then lines 1 and 2 for
    // each satellite. Each set must have valid checksums and the same number on both lines. A line
    // where a name goes that cannot be one (a stray line 2) counts as an entry that cannot be read,
    // rather than being taken as the next satellite's name.
    static std::vector<KeplerData> parseTleText(const std::vector<std::string> &lines, int &bad_entries)
    {
        std::vector<KeplerData> keps;
        size_t i = 0;
        while (i < lines.size())
        {
            std::string name;
            if (lines[i].rfind("1 ", 0) != 0)
            {
                if (notASatelliteName(lines[i]))
                {
                    bad_entries++;
                    i++;
                    continue;
                }
                name = lines[i].rfind("0 ", 0) == 0 ? lines[i].substr(2) : lines[i];
                i++;
            }

            if (i + 1 < lines.size() && lines[i].rfind("1 ", 0) == 0 && lines[i + 1].rfind("2 ", 0) == 0)
            {
                TLE tle;
                tle.name = name;
                tle.line1 = lines[i];
                tle.line2 = lines[i + 1];
                KeplerData kep;
                if (tleLineValid(tle.line1, '1') && tleLineValid(tle.line2, '2') && tle.line1.compare(2, 5, tle.line2, 2, 5) == 0 && tleToKepler(tle, kep))
                {
                    if (kep.name.empty())
                        kep.name = std::to_string(kep.satellite_number);
                    keps.push_back(kep);
                }
                else
                    bad_entries++;
                i += 2;
            }
            else
            {
                bad_entries++; // a name with no lines after it, or a line 1 with no line 2
                if (name.empty())
                    i++;
            }
        }
        return keps;
    }

    std::vector<KeplerData> parseKeplerText(const std::string &text, int &bad_entries)
    {
        bad_entries = 0;
        std::vector<std::string> lines = trimmedLines(text);

        // TLE text when a line 1 is followed by a line 2 anywhere; OMM CSV otherwise
        for (size_t i = 0; i + 1 < lines.size(); i++)
            if (lines[i].rfind("1 ", 0) == 0 && lines[i + 1].rfind("2 ", 0) == 0)
                return parseTleText(lines, bad_entries);

        std::vector<KeplerData> keps;
        for (auto &line : lines)
        {
            if (line.rfind("OBJECT_NAME", 0) == 0) // the header line, naming the columns
                continue;
            KeplerData kep;
            if (ccsdsOmmToKepler(line, kep))
                keps.push_back(kep);
            else
                bad_entries++;
        }
        return keps;
    }

    std::vector<KeplerData> readKeplerFile(const std::string &url_str)
    {
        std::string content, error;
        if (!readFileUrl(url_str, content, error))
        {
            logger->warn("Cannot read the orbit element file %s (%s); the elements already loaded are kept", url_str.c_str(), error.c_str());
            return {};
        }

        int bad_entries = 0;
        std::vector<KeplerData> keps = parseKeplerText(content, bad_entries);
        if (bad_entries > 0)
        {
            logger->warn("The orbit element file %s has %d entries that cannot be read, so none of it is used; the elements already loaded are kept", url_str.c_str(), bad_entries);
            return {};
        }
        if (keps.empty())
        {
            logger->warn("The orbit element file %s holds no elements; the elements already loaded are kept", url_str.c_str());
            return {};
        }

        logger->info("Read %zu orbit elements from %s", keps.size(), url_str.c_str());
        return keps;
    }

    std::vector<KeplerData> tryFetchOMMFileFromURL(std::string url_str)
    {
        // Fork: a local file is read once; a missing one is not tried again ten times
        if (isFileUrl(url_str))
            return readKeplerFile(url_str);

        bool success = true;
        std::vector<KeplerData> new_registry;

        logger->info(url_str);
        std::string result;
        int http_res = 1, trials = 0;
        while (http_res == 1 && trials < 10)
        {
            if ((http_res = perform_http_request(url_str, result)) != 1)
            {
                int bad_entries = 0; // Fork: TLE text is read too, not only OMM CSV
                new_registry = parseKeplerText(result, bad_entries);
                success = new_registry.size() > 0;
            }
            else
                success = false;
            trials++;
            if (!success)
            {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                logger->info("Failed getting OMMs. Retrying...");
            }
        }

        if (!success)
            logger->warn("Failed to get OMM for %s", url_str.c_str());

        return new_registry;
    }

    std::vector<KeplerData> tryFetchSingleOMMwithNorad(int norad)
    {
        bool success = true;
        std::vector<KeplerData> new_registry;

        std::string url_str = satdump_cfg.main_cfg["kepler_settings"]["url_template"].get<std::string>();
        while (url_str.find("%NORAD%") != std::string::npos)
            url_str.replace(url_str.find("%NORAD%"), 7, std::to_string(norad));

        logger->info(url_str);
        std::string result;
        int http_res = 1, trials = 0;
        while (http_res == 1 && trials < 10)
        {
            if ((http_res = perform_http_request(url_str, result)) != 1)
            {
                new_registry = parseCcsdsOmmFile(result);
                success = new_registry.size() > 0;
            }
            else
                success = false;
            trials++;
            if (!success)
            {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                logger->info("Failed getting Kepler. Retrying...");
            }
        }

        if (!success)
            logger->error("Error updating Kepler for %d. Ignoring!", norad);

        return new_registry;
    }
} // namespace satdump