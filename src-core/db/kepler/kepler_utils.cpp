#include "core/config.h"
#include "kepler_handler.h"
#include "libs/predict/predict.h"
#include "logger.h"
#include "utils/file_url.h"
#include "utils/http.h"
#include "utils/string.h"
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include "common/utils.h"

namespace satdump
{
    // Fork: the TLE lines are written field by field below, each value in its own columns of the
    // TLE layout, rounded (halves away from zero) and checked to fit. The code before cut values
    // off instead of rounding them (an inclination of 98.69 became "98.68999", a first derivative
    // of 0.000001 ".000000999"), left no column for the sign of the first derivative, wrote spaces
    // where a TLE has zeros (catalogue number 5 as "    5", the year 2005 as " 5"), wrote "C" for
    // both checksums, and wrote the drag term (BSTAR) and second derivative wrong for most values
    // (strLE)
    namespace
    {
        // Fork: a finite number above 0 as the shortest decimal that reads back as the same double,
        // which is the number a TLE or OMM file gave: its significant digits, and the power of ten
        // that puts the decimal point in front of them (0.00015 is "15" and -3). Rounding this
        // decimal, rather than the binary value just under it, rounds 0.0000999995 up, as written
        void shortestDecimal(double a, std::string &digits, int &exp10)
        {
            char text[64];
            for (int precision = 0; precision <= 16; precision++) // 17 digits always read back
            {
                snprintf(text, sizeof(text), "%.*e", precision, a);
                if (strtod(text, nullptr) == a)
                    break;
            }
            // "d.dddde-05": the digits before the 'e', whatever decimal point the locale uses
            const char *c = text;
            digits.clear();
            for (; *c != 'e' && *c != '\0'; c++)
                if (isdigit((unsigned char)*c))
                    digits += *c;
            exp10 = (*c == 'e' ? atoi(c + 1) : 0) + 1;
            while (digits.size() > 1 && digits.back() == '0')
                digits.pop_back();
        }

        // Fork: a finite a >= 0 rounded to `decimals` places after the point, halves away from
        // zero, as the digits of one whole number ("0" when it rounds to 0): 98.69 to 4 places is
        // "986900", 0.00015 to 7 places is "1500". `decimals` may be negative
        std::string roundedDigits(double a, int decimals)
        {
            if (a == 0)
                return "0";
            std::string digits;
            int exp10;
            shortestDecimal(a, digits, exp10);

            // The digits that stay: those in front of the point, then `decimals` more
            int keep = exp10 + decimals;
            if (keep < 0)
                return "0";
            std::string result;
            bool up = false;
            if ((size_t)keep < digits.size())
            {
                result = digits.substr(0, keep);
                up = digits[keep] >= '5';
            }
            else
                result = digits + std::string(keep - digits.size(), '0');
            if (up)
            {
                int i = (int)result.size() - 1;
                while (i >= 0 && result[i] == '9')
                    result[i--] = '0';
                if (i >= 0)
                    result[i]++;
                else
                    result.insert(result.begin(), '1');
            }
            size_t first = result.find_first_not_of('0');
            return first == std::string::npos ? "0" : result.substr(first);
        }

        // Fork: v with `decimals` places after the point, right-aligned in a column `width` wide,
        // with a '-' in front when it is below 0 (none for one that rounds to 0). False when it
        // is wider than the column
        bool fixedField(double v, int width, int decimals, std::string &out)
        {
            std::string n = roundedDigits(std::fabs(v), decimals);
            if (n.size() < (size_t)decimals + 1)
                n.insert(0, decimals + 1 - n.size(), '0');
            std::string text = n.substr(0, n.size() - decimals) + "." + n.substr(n.size() - decimals);
            if (v < 0 && n.find_first_not_of('0') != std::string::npos)
                text.insert(0, "-");
            if (text.size() > (size_t)width)
                return false;
            out += std::string(width - text.size(), ' ') + text;
            return true;
        }

        // Fork: a whole number right-aligned in a column `width` wide, padded with zeros or
        // spaces. False when it is wider than the column
        bool wholeField(long long v, int width, bool zeros, std::string &out)
        {
            char text[64];
            if (zeros)
                snprintf(text, sizeof(text), "%0*lld", width, v);
            else
                snprintf(text, sizeof(text), "%*lld", width, v);
            if (strlen(text) > (size_t)width)
                return false;
            out += text;
            return true;
        }

        // Fork: the first derivative of the mean motion: a sign column (' ' or '-'), then a
        // point and 8 decimals. False when it rounds to 1 or more in size
        bool firstDerivativeField(double v, std::string &out)
        {
            std::string n = roundedDigits(std::fabs(v), 8);
            if (n.size() > 8)
                return false;
            out += (v < 0 && n != "0") ? "-." : " .";
            out += std::string(8 - n.size(), '0') + n;
            return true;
        }

        // Fork: the eccentricity as its 7 decimals, the point left out. False when it rounds to 1
        // or more, or to less than 0 (the column has no sign)
        bool eccentricityField(double v, std::string &out)
        {
            std::string n = roundedDigits(std::fabs(v), 7);
            if (n.size() > 7 || (v < 0 && n != "0"))
                return false;
            out += std::string(7 - n.size(), '0') + n;
            return true;
        }

        // Fork: the epoch: the last two digits of the year, then the day of the year with 8
        // decimals (1.0 is 1 January, 00:00 UTC), rounded; a time that rounds to the next year
        // is day 1.0 of that year. False when the year is not 2000 to 2099 once rounded, as a TLE
        // year is read back as 20yy
        bool epochField(double epoch, std::string &out)
        {
            // A day either side of those years, so the times below are those of a time_t
            if (!(epoch >= 946684800.0 - 86400.0 && epoch < 4102444800.0 + 86400.0))
                return false;
            // Day 0 of January of a year: 31 December before it, so 1 January is day 1
            auto day_zero = [](int tm_year) {
                tm start; // timegm changes the tm it is given, so each call has a new one
                memset(&start, 0, sizeof(start));
                start.tm_year = tm_year;
                return (double)timegm(&start);
            };
            // The year the epoch is in, found with timegm alone: gmtime, which the code before
            // used, fills one tm shared by every thread. The guess from the mean length of a year
            // is off by one year at most
            int year = 100 + (int)std::floor((epoch - 946684800.0) / (365.2425 * 86400.0));
            while (day_zero(year) + 86400.0 > epoch)
                year--;
            while (day_zero(year + 1) + 86400.0 <= epoch)
                year++;
            double day0 = day_zero(year);
            long long year_days = std::llround((day_zero(year + 1) - day0) / 86400.0);

            const long long unit = 100000000; // 1e-8 day, the last decimal of the day
            long long day = std::stoll(roundedDigits((epoch - day0) / 86400.0, 8));
            if (day >= (year_days + 1) * unit)
            {
                year++;
                day -= year_days * unit;
            }
            if (year < 100 || year > 199)
                return false;
            char text[64];
            snprintf(text, sizeof(text), "%02d%03lld.%08lld", year - 100, day / unit, day % unit);
            out += text;
            return true;
        }

        // Fork: the checksum of a TLE line: its digits added up, with 1 for each '-', modulo 10
        char tleChecksum(const std::string &l)
        {
            int sum = 0;
            for (size_t i = 0; i < l.size() && i < 68; i++)
            {
                if (l[i] == '-')
                    sum++;
                else if (isdigit((unsigned char)l[i]))
                    sum += l[i] - '0';
            }
            return (char)('0' + sum % 10);
        }

        // Fork: whether a designator has the OMM form, YYYY-NNN then 1 to 3 letters for the piece
        bool isOmmDesignator(const std::string &d)
        {
            if (d.size() < 9 || d.size() > 11 || d[4] != '-')
                return false;
            for (size_t i = 0; i < d.size(); i++)
                if (i != 4 && !(i < 8 ? isdigit((unsigned char)d[i]) : isalpha((unsigned char)d[i])))
                    return false;
            return true;
        }

        // Fork: lines 1 and 2 of the TLE for these elements and "", or (the lines then unfinished)
        // why a TLE cannot hold them. keplerTleProblem and keplerToTle both use it, so what is
        // refused is exactly what cannot be written
        std::string writeTleLines(const KeplerData &kep, std::string &line1, std::string &line2)
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

            auto columns = [](const char *name, int width) { return std::string("the ") + name + " does not fit the " + std::to_string(width) + " columns a TLE gives it"; };

            // Above 99999, the catalogue number is written as 99999, as it always was
            std::string number;
            if (!wholeField(kep.satellite_number > 99999 ? 99999 : kep.satellite_number, 5, true, number))
                return columns("catalogue number", 5);

            // An OMM designator ("2019-074AB") is written in TLE form ("19074AB "). Only the one
            // with a one-letter piece was, and longer ones were cut to "2019-074". Every designator
            // is padded with spaces to 8, as in a TLE; resize(8) padded the shorter ones TLE text
            // gives ("12049A") with NUL characters
            std::string des = kep.designator;
            if (isOmmDesignator(des))
                des = des.substr(2, 2) + des.substr(5, 3) + des.substr(8);
            des.resize(8, ' ');

            line1 = "1 " + number + "U " + des + " ";
            if (!epochField(kep.epoch, line1))
                return "the epoch is not in the years 2000 to 2099";
            line1 += " ";
            if (!firstDerivativeField(kep.derivative_mean_motion, line1))
                return "the first derivative of the mean motion is 1 or more in size once rounded to 8 decimals, more than a TLE holds";
            const std::pair<const char *, double> exponent_values[] = {{"second derivative of the mean motion", kep.second_derivative_mean_motion},
                                                                       {"drag term (BSTAR)", kep.bstar_drag_term}};
            for (auto &v : exponent_values)
            {
                std::optional<std::string> field = strLE(v.second);
                if (!field)
                    return std::string("the ") + v.first + " is 1 or more in size once rounded to five digits, more than a TLE holds";
                line1 += " " + *field;
            }
            line1 += " 0 "; // the ephemeris type, always 0
            // The element set number and revolution number are counters: their last digits are kept
            if (!wholeField(kep.element_number % 10000, 4, false, line1))
                return columns("element set number", 4);
            line1 += tleChecksum(line1);

            line2 = "2 " + number;
            const std::pair<const char *, double> angles[] = {{"inclination", kep.inclination}, {"right ascension", kep.right_ascension}};
            for (auto &a : angles)
            {
                line2 += " ";
                if (!fixedField(a.second, 8, 4, line2))
                    return columns(a.first, 8);
            }
            line2 += " ";
            if (!eccentricityField(kep.eccentricity, line2))
                return "the eccentricity is not from 0 to 0.9999999 once rounded to 7 decimals, as a TLE holds it";
            const std::pair<const char *, double> more_angles[] = {{"argument of perigee", kep.argument_of_perigee}, {"mean anomaly", kep.mean_anomaly}};
            for (auto &a : more_angles)
            {
                line2 += " ";
                if (!fixedField(a.second, 8, 4, line2))
                    return columns(a.first, 8);
            }
            line2 += " ";
            if (!fixedField(kep.mean_motion, 11, 8, line2))
                return columns("mean motion", 11);
            if (!wholeField(kep.revolutions_at_epoch % 100000, 5, false, line2))
                return columns("revolution number", 5);
            line2 += tleChecksum(line2);
            return "";
        }
    } // namespace

    // Fork: see kepler_handler.h. It wrote "12345-15" for 1.2345e-05 and "    5--1" for 0.0005:
    // it multiplied by 10 until no fraction was left, which for most doubles only happens near
    // 2^53, and threw at 1 and above
    std::optional<std::string> strLE(double val)
    {
        if (!std::isfinite(val))
            return std::nullopt;
        std::string mantissa = "00000";
        int exponent = 0;
        if (val != 0)
        {
            std::string digits;
            int exp10; // |val| = 0.digits x 10^exp10
            shortestDecimal(std::fabs(val), digits, exp10);
            std::string five = roundedDigits(std::fabs(val), 5 - exp10);
            if (five.size() > 5) // 0.999995 rounds to 1.0000: one more digit
            {
                five.resize(5);
                exp10++;
            }
            if (exp10 > 0)
                return std::nullopt; // 1 or more
            if (exp10 >= -9)
            {
                mantissa = five;
                exponent = -exp10;
            }
            else
            {
                // Below 1e-10 the exponent stays at 9 and the digits move right, down to 00001-9
                // (1e-14); what is smaller than half of that is 0
                std::string units = roundedDigits(std::fabs(val), 14);
                if (units != "0")
                {
                    mantissa = std::string(5 - units.size(), '0') + units;
                    exponent = 9;
                }
            }
        }
        std::string sign = val < 0 && mantissa != "00000" ? "-" : " ";
        return sign + mantissa + "-" + std::to_string(exponent);
    }

    // Fork: see kepler_handler.h
    std::string keplerTleProblem(const KeplerData &kep)
    {
        std::string line1, line2;
        return writeTleLines(kep, line1, line2);
    }

    TLE keplerToTle(KeplerData kep)
    {
        TLE tle;
        // Fork: values a TLE cannot hold are an error, never a hang or a wrong line (see above)
        std::string problem = writeTleLines(kep, tle.line1, tle.line2);
        if (!problem.empty())
            throw satdump_exception("the elements of satellite " + std::to_string(kep.satellite_number) + " cannot be written as a TLE: " + problem);
        tle.name = kep.name;
        tle.norad = kep.satellite_number;
        tle.time = kep.epoch;
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
        return l.size() == 69 && l[0] == number && l[1] == ' ' && l[68] == tleChecksum(l);
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

    // Fork: the elements in a downloaded text (OMM CSV or TLE text). Entries that cannot be read,
    // values a TLE cannot hold among them, are skipped with one warning that counts them. They
    // were dropped without a word, and their satellites kept the elements already loaded
    static std::vector<KeplerData> parseDownloadedKeplerText(const std::string &text, const std::string &url_str)
    {
        int bad_entries = 0;
        std::vector<KeplerData> keps = parseKeplerText(text, bad_entries);
        if (bad_entries > 0)
            logger->warn("%d of the orbit element entries downloaded from %s cannot be read, so they are skipped; the elements already loaded are kept for them",
                         bad_entries, url_str.c_str());
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
            // Fork: each try starts from nothing; a try that failed part way left its text, and
            // the next one was added to it
            result.clear();
            if ((http_res = perform_http_request(url_str, result)) != 1)
            {
                // Fork: TLE text is read too, not only OMM CSV, and entries that cannot be read are reported
                new_registry = parseDownloadedKeplerText(result, url_str);
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
            // Fork: each try starts from nothing; a try that failed part way left its text, and
            // the next one was added to it
            result.clear();
            if ((http_res = perform_http_request(url_str, result)) != 1)
            {
                // Fork: as above (parseCcsdsOmmFile dropped such entries without a word)
                new_registry = parseDownloadedKeplerText(result, url_str);
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