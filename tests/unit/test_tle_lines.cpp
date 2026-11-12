// Fork: the TLE lines SatDump writes for the orbit elements it stores (keplerToTle). Every element
// it reads (TLE text, OMM CSV, downloads) is given out through them, and pass prediction
// (predict_parse_tle) and products use them. Covers: TLE text read and written back giving the
// same lines, checksums included, for the TLEs the other tests use and for realistic lines with
// every exponent of the drag term (BSTAR) and second derivative, also through the database; OMM
// CSV giving the TLE lines of the same elements, also through the database; the 8 columns of
// those two values (strLE) on their own: rounding, the carry, the limits and zero; the orbit
// library reading the lines back to the same values; what a TLE cannot hold being refused
// (counters below 0 among it), and nothing else.

#include "db/kepler/kepler_handler.h"
#include "doctest.h"
#include "libs/predict/predict.h"
#include "offline_rig.h"
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    // The TLE text of the other tests: test_offline_data.cpp, test_passes.cpp and cli/offline.sh
    // (METOP-B, NOAA 19, METEOR-M2 3), and the designator that fills all 8 columns
    const char *TEST_TLES = "METOP-B\n"
                            "1 38771U 12049A   25314.50000000  .00000100  00000-0  50000-4 0  9992\n"
                            "2 38771  98.6900  10.1234 0001500  90.1000 270.0500 14.21512345678909\n"
                            "NOAA 19\n"
                            "1 33591U 09005A   25314.50000000  .00000100  00000-0  50000-4 0  9995\n"
                            "2 33591  99.0500  40.5000 0001400 100.2000 260.0000 14.13000000865436\n"
                            "METEOR-M2 3\n"
                            "1 57166U 23091A   25314.50000000  .00000100  00000-0  50000-4 0  9990\n"
                            "2 57166  98.6500  20.7500 0003000  80.5000 279.6000 14.24000000123455\n"
                            "DEBRIS\n"
                            "1 31141U 99025BGQ 25314.50000000  .00000100  00000-0  50000-4 0  9995\n"
                            "2 31141  98.6900  10.1234 0001500  90.1000 270.0500 14.21512345678903\n";

    // Realistic lines. The first five are real ones (Vanguard 1, the ISS, the Spacetrack Report
    // No. 3 test set and two of the SGP4 verification set); the others are made up so that the
    // drag term (BSTAR) and the second derivative each have every exponent from 0 to 9, both
    // signs, the mantissas 00000, 10000 and 99999, and the first derivative, eccentricity,
    // angles, mean motion, epoch and counters their smallest and largest values
    const char *REALISTIC_TLES = "VANGUARD 1\n"
                                 "1 00005U 58002B   00179.78495062  .00000023  00000-0  28098-4 0  4753\n"
                                 "2 00005  34.2682 348.7242 1859667 331.7664  19.3264 10.82419157413667\n"
                                 "ISS (ZARYA)\n"
                                 "1 25544U 98067A   08264.51782528 -.00002182  00000-0 -11606-4 0  2927\n"
                                 "2 25544  51.6416 247.4627 0006703 130.5360 325.0288 15.72125391563537\n"
                                 "SPACETRACK 88888\n"
                                 "1 88888U          80275.98708465  .00073094  13844-3  66816-4 0    87\n"
                                 "2 88888  72.8435 115.9689 0086731  52.6988 110.5714 16.05824518  1058\n"
                                 "DECAYING 29141\n"
                                 "1 29141U 85108AA  06170.26783845  .99999999  00000-0  13519-0 0   718\n"
                                 "2 29141  82.4288 273.4882 0015848 277.2124  83.9133 15.93343074  6828\n"
                                 "GTO 23599\n"
                                 "1 23599U 95029B   06171.76535463  .00085586  12891-6  12956-2 0  2905\n"
                                 "2 23599   6.9327   0.2849 5782022 274.4436  25.2425  4.47796565123555\n"
                                 "GEO 41866\n"
                                 "1 41866U 16071A   25314.12345678 -.00000097  00000-0  00000-0 0  9998\n"
                                 "2 41866   0.0130 274.0570 0000001 288.8919 109.2700  1.00271131262366\n"
                                 "ZERO ECC\n"
                                 "1 41867U 16071B   25001.00000000  .00000000 -99999-1  10000-9 0    10\n"
                                 "2 41867   0.0000   0.0000 0000000   0.0000   0.0000  1.00000000    09\n"
                                 "NEAR ONE ECC\n"
                                 "1 41868U 16071C   24366.99999999 -.99999999  10000-2 -99999-0 0 99996\n"
                                 "2 41868 180.0000 359.9999 9999999 359.9999 359.9999  0.29008891999992\n"
                                 "EXP 1 5\n"
                                 "1 40001U 14001A   25100.50000000  .00012345  31416-1  27183-5 0   127\n"
                                 "2 40001  97.8000 100.0000 0012345  90.0000 270.0000 14.80000000    19\n"
                                 "EXP 2 6\n"
                                 "1 40002U 14001B   25100.50000000 -.00012345 -50505-2 -14142-6 0  1237\n"
                                 "2 40002  97.8000 100.0000 0012345  90.0000 270.0000 14.80000000   122\n"
                                 "EXP 4 7\n"
                                 "1 40003U 14001C   25100.50000000  .00001000  12345-4  17320-7 0  4564\n"
                                 "2 40003  97.8000 100.0000 0012345  90.0000 270.0000 14.80000000  1236\n"
                                 "EXP 5 8\n"
                                 "1 40004U 14001D   25100.50000000  .00010000  22222-5 -33333-8 0  7894\n"
                                 "2 40004  97.8000 100.0000 0012345  90.0000 270.0000 14.80000000 12341\n"
                                 "EXP 7 9\n"
                                 "1 40005U 14001E   25100.50000000  .10000000 -44444-7  55555-9 0  9991\n"
                                 "2 40005  97.8000 100.0000 0012345  90.0000 270.0000 14.80000000123457\n"
                                 "EXP 8 3\n"
                                 "1 40006U 14001F   25100.50000000 -.00000001  66666-8  50000-3 0 10001\n"
                                 "2 40006  97.8000 100.0000 0012345  90.0000 270.0000 14.80000000543218\n"
                                 "EXP 9 3\n"
                                 "1 40007U 14001G   25100.50000000  .00000001  77777-9  10270-3 0    23\n"
                                 "2 40007  97.8000 100.0000 0012345  90.0000 270.0000 14.80000000    60\n"
                                 "EXP 0 1\n"
                                 "1 40008U 14001H   99365.99999999  .00000100 -12345-0  99999-1 0    31\n"
                                 "2 40008  97.8000 100.0000 0012345  90.0000 270.0000 14.80000000    72\n";

    // OMM CSV, as the default sources (CelesTrak, FORMAT=csv) and local OMM files give elements:
    // the header line, then satellites of the TLE text above, each value written as such files
    // write it (".00015", "-.11606E-4", "2.8098e-05", the catalogue number 5 as "5"), the epoch
    // with microseconds. Each value is read with std::stod, not from the columns of a TLE, so
    // this is a different way into the lines written. The first entry is test_offline_data.cpp's
    // OMM_METOP, the first set of TEST_TLES
    const char *OMM_TEXT = "OBJECT_NAME,OBJECT_ID,EPOCH,MEAN_MOTION,ECCENTRICITY,INCLINATION,RA_OF_ASC_NODE,ARG_OF_PERICENTER,MEAN_ANOMALY,"
                           "EPHEMERIS_TYPE,CLASSIFICATION_TYPE,NORAD_CAT_ID,ELEMENT_SET_NO,REV_AT_EPOCH,BSTAR,MEAN_MOTION_DOT,MEAN_MOTION_DDOT\n"
                           "METOP-B,2012-049A,2025-11-10T12:00:00.000000,14.21512345,.00015,98.69,10.1234,90.1,270.05,0,U,38771,999,67890,.00005,.000001,0\n"
                           "VANGUARD 1,1958-002B,2000-06-27T18:50:19.733568,10.82419157,.1859667,34.2682,348.7242,331.7664,19.3264,0,U,5,475,41366,2.8098e-05,2.3e-07,0\n"
                           "ISS (ZARYA),1998-067A,2008-09-20T12:25:40.104192,15.72125391,.0006703,51.6416,247.4627,130.536,325.0288,0,U,25544,292,56353,-.11606E-4,-.00002182,0\n"
                           "DECAYING 29141,1985-108AA,2006-06-19T06:25:41.242080,15.93343074,.0015848,82.4288,273.4882,277.2124,83.9133,0,U,29141,71,682,.13519,.99999999,0\n"
                           "GTO 23599,1995-029B,2006-06-20T18:22:06.640032,4.47796565,.5782022,6.9327,.2849,274.4436,25.2425,0,U,23599,290,12355,.0012956,.00085586,1.2891e-07\n"
                           "EXP 2 6,2014-001B,2025-04-10T12:00:00.000000,14.8,.0012345,97.8,100,90,270,0,U,40002,123,12,-1.4142E-07,-.00012345,-.0050505\n"
                           "EXP 8 3,2014-001F,2025-04-10T12:00:00.000000,14.8,.0012345,97.8,100,90,270,0,U,40006,1000,54321,.0005,-1e-08,6.6666e-09\n"
                           "EXP 0 1,2014-001H,2099-12-31T23:59:59.999136,14.8,.0012345,97.8,100,90,270,0,U,40008,3,7,.099999,.000001,-.12345\n";

    // A folder of the test's own, removed with everything in it at the end
    struct TempDir
    {
        fs::path path;
        TempDir()
        {
            std::string tmpl = (fs::temp_directory_path() / "satdump-tle-XXXXXX").string();
            REQUIRE(mkdtemp(tmpl.data()) != nullptr);
            path = tmpl;
        }
        ~TempDir()
        {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
        std::string write(const std::string &name, const std::string &content) const
        {
            std::ofstream(path / name, std::ios::binary) << content;
            return "file://" + (path / name).string();
        }
    };

    // A TLE line from its first 68 characters, with its checksum added
    std::string tle_line(const std::string &first68)
    {
        REQUIRE(first68.size() == 68);
        int sum = 0;
        for (char ch : first68)
            sum += ch == '-' ? 1 : (ch >= '0' && ch <= '9' ? ch - '0' : 0);
        return first68 + char('0' + sum % 10);
    }

    // The sets of a TLE text: name, line 1, line 2
    std::vector<std::array<std::string, 3>> tle_sets(const std::string &text)
    {
        std::vector<std::array<std::string, 3>> sets;
        std::istringstream in(text);
        std::array<std::string, 3> set;
        while (std::getline(in, set[0]) && std::getline(in, set[1]) && std::getline(in, set[2]))
            sets.push_back(set);
        return sets;
    }

    // The set with this name in TEST_TLES or REALISTIC_TLES
    std::array<std::string, 3> tle_set_named(const std::string &name)
    {
        for (const char *text : {TEST_TLES, REALISTIC_TLES})
            for (auto &set : tle_sets(text))
                if (set[0] == name)
                    return set;
        FAIL("no TLE set named " << name);
        return {};
    }

    // The entry lines of OMM_TEXT, the header left out
    std::vector<std::string> omm_entries()
    {
        std::vector<std::string> entries;
        std::istringstream in(OMM_TEXT);
        std::string line;
        std::getline(in, line);
        while (std::getline(in, line))
            entries.push_back(line);
        return entries;
    }

    // The elements SatDump reads from a set's two lines
    satdump::KeplerData read_elements(const std::string &line1, const std::string &line2)
    {
        int bad = -1;
        std::vector<satdump::KeplerData> keps = satdump::parseKeplerText(line1 + "\n" + line2 + "\n", bad);
        REQUIRE(bad == 0);
        REQUIRE(keps.size() == 1);
        return keps[0];
    }

    // What the orbit library reads from a TLE's lines
    std::unique_ptr<predict_orbital_elements_t, decltype(&predict_destroy_orbital_elements)> library_reads(const satdump::TLE &tle)
    {
        std::unique_ptr<predict_orbital_elements_t, decltype(&predict_destroy_orbital_elements)> elements(predict_parse_tle(tle.line1.c_str(), tle.line2.c_str()),
                                                                                                            &predict_destroy_orbital_elements);
        REQUIRE(elements != nullptr);
        return elements;
    }

    // a and b are the same, or differ by less than 1e-5 of a
    bool same_value(double a, double b) { return a == b || std::fabs(b - a) < 1e-5 * std::fabs(a); }

    // METOP-B's elements, as read from TEST_TLES
    satdump::KeplerData metop_elements()
    {
        std::vector<std::array<std::string, 3>> sets = tle_sets(TEST_TLES);
        return read_elements(sets[0][1], sets[0][2]);
    }

    // The TLE for these elements, which must be one that can be written
    satdump::TLE writable(const satdump::KeplerData &kep)
    {
        CHECK(satdump::keplerTleProblem(kep) == "");
        satdump::TLE tle;
        REQUIRE_NOTHROW(tle = satdump::keplerToTle(kep));
        CHECK(tle.line1.size() == 69);
        CHECK(tle.line2.size() == 69);
        CHECK(tle.line1[68] == tle_line(tle.line1.substr(0, 68))[68]);
        CHECK(tle.line2[68] == tle_line(tle.line2.substr(0, 68))[68]);
        return tle;
    }

    // Elements that cannot be written: the problem names the value, and keplerToTle throws
    void refused(const satdump::KeplerData &kep, const std::string &value)
    {
        CHECK(satdump::keplerTleProblem(kep).find("the " + value + " ") == 0);
        CHECK_THROWS_AS(satdump::keplerToTle(kep), std::exception);
    }
} // namespace

TEST_CASE("TLE lines: TLE text read and written back gives the same lines, checksums included")
{
    // Before, few lines came back as they were: 50000-4 was written "    5--1", .00000100 as
    // ".000000999", 98.6900 as "98.68999", the catalogue number 5 as "    5", and both
    // checksums as "C"
    for (const char *text : {TEST_TLES, REALISTIC_TLES})
    {
        for (auto &set : tle_sets(text))
        {
            CAPTURE(set[1]);
            CAPTURE(set[2]);
            satdump::KeplerData kep = read_elements(set[1], set[2]);
            satdump::TLE tle = satdump::keplerToTle(kep);
            CHECK(tle.line1 == set[1]);
            CHECK(tle.line2 == set[2]);
            CHECK(tle.norad == kep.satellite_number);
        }

        // All of it at once, as an element file is read
        int bad = -1;
        std::vector<satdump::KeplerData> keps = satdump::parseKeplerText(text, bad);
        CHECK(bad == 0);
        std::vector<std::array<std::string, 3>> sets = tle_sets(text);
        REQUIRE(keps.size() == sets.size());
        for (size_t i = 0; i < keps.size(); i++)
        {
            satdump::TLE tle = satdump::keplerToTle(keps[i]);
            CHECK(tle.name == sets[i][0]);
            CHECK(tle.line1 == sets[i][1]);
            CHECK(tle.line2 == sets[i][2]);
        }
    }

    // The TLE of the golden recording's admin message (tests/golden/make_recordings.py) and of
    // test_admin_msg.cpp writes the revolution number 0 as " 0000"; it comes back as TLEs write
    // it, "    0", the same number with the same checksum. Everything else is as it was
    for (const std::string epoch : {"25314.50000000", "25313.50000000"})
    {
        const std::string line1 = tle_line("1 38771U 12049A   " + epoch + "  .00000000  00000-0  00000-0 0  999");
        const std::string line2 = tle_line("2 38771  98.7000 300.0000 0001000  90.0000 270.0000 14.21000000 0000");
        satdump::TLE tle = satdump::keplerToTle(read_elements(line1, line2));
        CHECK(tle.line1 == line1);
        CHECK(tle.line2 == line2.substr(0, 63) + "    0" + line2[68]);
    }

    // CelesTrak writes a 0 drag term or second derivative as " 00000+0". It is read as 0, and
    // written back as " 00000-0", as Space-Track, the TLEs here and SatDump always wrote it, with
    // the checksum that goes with it
    const std::string celestrak = tle_line("1 41866U 16071A   25314.12345678 -.00000097  00000+0  00000+0 0  999");
    const std::string line2 = "2 41866   0.0130 274.0570 0000001 288.8919 109.2700  1.00271131262366";
    satdump::KeplerData kep = read_elements(celestrak, line2);
    CHECK(kep.bstar_drag_term == 0);
    CHECK(kep.second_derivative_mean_motion == 0);
    satdump::TLE tle = satdump::keplerToTle(kep);
    CHECK(tle.line1 == "1 41866U 16071A   25314.12345678 -.00000097  00000-0  00000-0 0  9998");
    CHECK(tle.line2 == line2);
}

TEST_CASE("TLE lines: the TLE given out from the database is the TLE text that was read")
{
    TempDir dir;
    std::string url = dir.write("elements.tle", REALISTIC_TLES);
    test_rig::OfflineRig rig(test_rig::general("4 hours", false), test_rig::kepler_sources({url}));
    std::vector<std::array<std::string, 3>> sets = tle_sets(REALISTIC_TLES);
    REQUIRE(rig.keplers->getAllNewestKepler().size() == sets.size());
    for (auto &set : sets)
    {
        CAPTURE(set[1]);
        std::optional<satdump::TLE> tle = rig.keplers->get_from_norad(std::atoi(set[1].substr(2, 5).c_str()));
        REQUIRE(tle.has_value());
        CHECK(tle->name == set[0]);
        CHECK(tle->line1 == set[1]);
        CHECK(tle->line2 == set[2]);
    }
}

TEST_CASE("TLE lines: OMM CSV gives the TLE lines of the same elements, also through the database")
{
    // The default downloads and local OMM files: each entry gives exactly the lines of the TLE
    // text for the same satellite, checksums included. METOP-B is
    // "1 38771U 12049A   25314.50000000  .00000100  00000-0  50000-4 0  9992"
    // "2 38771  98.6900  10.1234 0001500  90.1000 270.0500 14.21512345678909"
    const std::string header = std::string(OMM_TEXT).substr(0, std::string(OMM_TEXT).find('\n') + 1);
    for (const std::string &entry : omm_entries())
    {
        CAPTURE(entry);
        int bad = -1;
        std::vector<satdump::KeplerData> keps = satdump::parseKeplerText(header + entry + "\n", bad);
        CHECK(bad == 0);
        REQUIRE(keps.size() == 1);
        std::array<std::string, 3> set = tle_set_named(keps[0].name);
        satdump::TLE tle = satdump::keplerToTle(keps[0]);
        CHECK(tle.line1 == set[1]);
        CHECK(tle.line2 == set[2]);
    }

    // The whole file, as SatDump reads it at start-up and gives it out from its database
    TempDir dir;
    std::string url = dir.write("elements.csv", OMM_TEXT);
    test_rig::OfflineRig rig(test_rig::general("4 hours", false), test_rig::kepler_sources({url}));
    std::vector<std::string> entries = omm_entries();
    REQUIRE(rig.keplers->getAllNewestKepler().size() == entries.size());
    for (const std::string &entry : entries)
    {
        CAPTURE(entry);
        std::array<std::string, 3> set = tle_set_named(entry.substr(0, entry.find(',')));
        std::optional<satdump::TLE> tle = rig.keplers->get_from_norad(std::atoi(set[1].substr(2, 5).c_str()));
        REQUIRE(tle.has_value());
        CHECK(tle->name == set[0]);
        CHECK(tle->line1 == set[1]);
        CHECK(tle->line2 == set[2]);
    }
}

TEST_CASE("TLE lines: the drag term and second derivative columns (strLE)")
{
    const std::vector<std::pair<double, std::string>> cases = {
        // Values the old strLE got wrong ("12345-15", "    5--1", "    1--1", "  1027-2",
        // "12345-21", "000000-0") and some it got right
        {1.2345e-05, " 12345-4"},
        {0.0005, " 50000-3"},
        {0.0001, " 10000-3"},
        {0.0001027, " 10270-3"},
        {1.2345e-10, " 12345-9"},
        {0, " 00000-0"},
        {3.4567e-06, " 34567-5"},
        {-1.1606e-05, "-11606-4"},
        {2.8098e-05, " 28098-4"},
        // Every exponent
        {0.5, " 50000-0"},
        {0.05, " 50000-1"},
        {0.005, " 50000-2"},
        {5e-6, " 50000-5"},
        {5e-7, " 50000-6"},
        {5e-8, " 50000-7"},
        {5e-9, " 50000-8"},
        {5e-10, " 50000-9"},
        // The largest and smallest of an exponent
        {0.99999, " 99999-0"},
        {0.1, " 10000-0"},
        {0.099999, " 99999-1"},
        {9.9999e-10, " 99999-9"},
        {1e-10, " 10000-9"},
        {-0.99999, "-99999-0"},
        // Five significant digits, halves away from zero (of the decimal the value was given as)
        {1.234549e-5, " 12345-4"},
        {1.23455e-5, " 12346-4"},
        {-1.23455e-5, "-12346-4"},
        {0.123456789, " 12346-0"},
        {-3.14159265e-7, "-31416-6"},
        // A carry to the next exponent
        {0.999995e-4, " 10000-3"},
        {-0.999995e-4, "-10000-3"},
        {0.0999996, " 10000-0"},
        {0.999995e-9, " 10000-8"},
        {0.999995e-10, " 10000-9"},
        {0.9999949e-4, " 99999-4"},
        {0.99999499, " 99999-0"},
        // Below 1e-10 the exponent stays at 9 and the digits move right, down to 1e-14; what
        // is smaller than half of that is 0, and a 0 has no sign
        {9.99994e-11, " 10000-9"},
        {5e-11, " 05000-9"},
        {-1.2345e-12, "-00123-9"},
        {1.2355e-12, " 00124-9"},
        {1e-14, " 00001-9"},
        {5e-15, " 00001-9"},
        {4.9999e-15, " 00000-0"},
        {-4e-15, " 00000-0"},
        {1e-300, " 00000-0"},
        {5e-324, " 00000-0"},
        {-0.0, " 00000-0"},
    };
    for (auto &c : cases)
    {
        CAPTURE(c.first);
        std::optional<std::string> field = satdump::strLE(c.first);
        REQUIRE(field.has_value());
        CHECK(*field == c.second);
    }

    // Sizes that round to 1 or more, and values that are not numbers, cannot be written
    for (double v : {0.999995, -0.999995, 1.0, -1.0, 2.5, 1e300, (double)NAN, (double)INFINITY, -(double)INFINITY})
    {
        CAPTURE(v);
        CHECK_FALSE(satdump::strLE(v).has_value());
    }
}

TEST_CASE("TLE lines: the orbit library reads the lines back to the same values")
{
    // Elements read from TLE text: the drag term and second derivative of the lines written
    // are those read (they were wrong by 10 or by many powers of ten)
    std::string old_errors;
    for (const std::string field : {" 12345-4", " 50000-3", " 10000-3", " 10270-3", " 12345-9", " 34567-5", "-11606-4", " 28098-4"})
    {
        const std::string line1 = tle_line("1 38771U 12049A   25314.50000000  .00000100 " + field + " " + field + " 0  999");
        old_errors += "METOP-B\n" + line1 + "\n" + "2 38771  98.6900  10.1234 0001500  90.1000 270.0500 14.21512345678909\n";
    }
    for (const char *text : {TEST_TLES, REALISTIC_TLES, old_errors.c_str()})
    {
        for (auto &set : tle_sets(text))
        {
            CAPTURE(set[1]);
            satdump::KeplerData kep = read_elements(set[1], set[2]);
            auto back = library_reads(satdump::keplerToTle(kep));
            CHECK(same_value(kep.bstar_drag_term, back->bstar_drag_term));
            CHECK(same_value(kep.second_derivative_mean_motion, back->second_derivative_mean_motion));
            CHECK(same_value(kep.derivative_mean_motion, back->derivative_mean_motion));
            CHECK(same_value(kep.eccentricity, back->eccentricity));
            CHECK(same_value(kep.mean_motion, back->mean_motion));
            CHECK(same_value(kep.inclination, back->inclination));
            CHECK(kep.satellite_number == back->satellite_number);
        }
    }

    // Any value: what is read back is the value rounded to the five digits written, so it is
    // off by at most half of the last digit
    satdump::KeplerData kep = metop_elements();
    for (double mantissa : {0.1, 0.123456789, 0.5, 0.987654321, 0.99999})
        for (int exponent = 0; exponent <= 9; exponent++)
            for (double sign : {1.0, -1.0})
            {
                kep.bstar_drag_term = sign * mantissa * std::pow(10.0, -exponent);
                kep.second_derivative_mean_motion = -kep.bstar_drag_term;
                CAPTURE(kep.bstar_drag_term);
                satdump::TLE tle = writable(kep);
                auto back = library_reads(tle);
                CHECK(tle.line1[60] - '0' == exponent);
                const double half_digit = 0.5e-5 * std::pow(10.0, -exponent) * (1 + 1e-9);
                CHECK(std::fabs(back->bstar_drag_term - kep.bstar_drag_term) <= half_digit);
                CHECK(std::fabs(back->second_derivative_mean_motion - kep.second_derivative_mean_motion) <= half_digit);
            }
}

TEST_CASE("TLE lines: what a TLE cannot hold is refused, and nothing else")
{
    struct Change
    {
        const char *value;   // as the problem names it
        double satdump::KeplerData::*member;
        double fits, too_big; // the largest that fits (written as `written`), and the next above
        const char *written;
        int line, column, width;
    };
    const std::vector<Change> changes = {
        {"drag term (BSTAR)", &satdump::KeplerData::bstar_drag_term, 0.99999499, 0.999995, " 99999-0", 1, 53, 8},
        {"drag term (BSTAR)", &satdump::KeplerData::bstar_drag_term, -0.99999499, -0.999995, "-99999-0", 1, 53, 8},
        {"second derivative of the mean motion", &satdump::KeplerData::second_derivative_mean_motion, 0.99999499, 0.999995, " 99999-0", 1, 44, 8},
        {"first derivative of the mean motion", &satdump::KeplerData::derivative_mean_motion, 0.999999994, 0.999999995, " .99999999", 1, 33, 10},
        {"first derivative of the mean motion", &satdump::KeplerData::derivative_mean_motion, -0.999999994, -0.999999995, "-.99999999", 1, 33, 10},
        {"inclination", &satdump::KeplerData::inclination, 999.99994, 999.99995, "999.9999", 2, 8, 8},
        {"inclination", &satdump::KeplerData::inclination, -99.99994, -99.99995, "-99.9999", 2, 8, 8},
        {"right ascension", &satdump::KeplerData::right_ascension, 999.99994, 999.99995, "999.9999", 2, 17, 8},
        {"eccentricity", &satdump::KeplerData::eccentricity, 0.99999994, 0.99999995, "9999999", 2, 26, 7},
        {"eccentricity", &satdump::KeplerData::eccentricity, -0.00000004, -0.00000005, "0000000", 2, 26, 7},
        {"argument of perigee", &satdump::KeplerData::argument_of_perigee, 999.99994, 999.99995, "999.9999", 2, 34, 8},
        {"mean anomaly", &satdump::KeplerData::mean_anomaly, -99.99994, -99.99995, "-99.9999", 2, 43, 8},
        {"mean motion", &satdump::KeplerData::mean_motion, 99.999999994, 99.999999995, "99.99999999", 2, 52, 11},
        {"mean motion", &satdump::KeplerData::mean_motion, -9.999999994, -9.999999995, "-9.99999999", 2, 52, 11},
    };
    for (auto &c : changes)
    {
        CAPTURE(c.value);
        CAPTURE(c.fits);
        satdump::KeplerData kep = metop_elements();
        kep.*c.member = c.fits;
        satdump::TLE tle = writable(kep);
        CHECK((c.line == 1 ? tle.line1 : tle.line2).substr(c.column, c.width) == c.written);
        kep.*c.member = c.too_big;
        refused(kep, c.value);
    }

    // The drag term (BSTAR) and second derivative: tiny values are written as 0, and never hang
    // (they used to be refused below 1e-14, as strLE went round its loop for ever below about
    // 1e-292); what is refused is what strLE cannot write
    for (double v : {1e-300, -1e-300, 5e-324, 1e-15, 0.0, -0.0})
    {
        CAPTURE(v);
        satdump::KeplerData kep = metop_elements();
        kep.bstar_drag_term = v;
        kep.second_derivative_mean_motion = v;
        satdump::TLE tle = writable(kep);
        CHECK(tle.line1.substr(44, 17) == " 00000-0  00000-0");
    }
    for (double v : {0.999995, 0.9999949, 0.5, 1e-5, 4.9e-15, 5e-15, 1.0, 2.5, -1.0, -0.999995, -0.9999949})
    {
        CAPTURE(v);
        satdump::KeplerData kep = metop_elements();
        kep.bstar_drag_term = v;
        CHECK(satdump::keplerTleProblem(kep).empty() == satdump::strLE(v).has_value());
        kep = metop_elements();
        kep.second_derivative_mean_motion = v;
        CHECK(satdump::keplerTleProblem(kep).empty() == satdump::strLE(v).has_value());
    }

    // The epoch: the years 2000 to 2099 once rounded to the 1e-8 day the TLE gives. A time just
    // before 2000 that rounds to its first moment can be written; a time that rounds to 2100 cannot
    satdump::KeplerData kep = metop_elements();
    kep.epoch = 946684800.0 - 0.0004;
    CHECK(writable(kep).line1.substr(18, 14) == "00001.00000000");
    kep.epoch = 946684800.0 - 0.0005;
    refused(kep, "epoch");
    kep.epoch = 4102444800.0 - 0.0005;
    CHECK(writable(kep).line1.substr(18, 14) == "99365.99999999");
    kep.epoch = 4102444800.0 - 0.0004;
    refused(kep, "epoch");
    // The day rounded, not cut: 0.4 ms before the next day of 2025 is that day; a time that rounds
    // to 1 January is day 1 of the next year, not day 366
    kep.epoch = 1762819200.0 - 0.0004; // 2025-11-11 00:00 UTC
    CHECK(writable(kep).line1.substr(18, 14) == "25315.00000000");
    kep.epoch = 1767225600.0 - 0.0004; // 2026-01-01 00:00 UTC
    CHECK(writable(kep).line1.substr(18, 14) == "26001.00000000");
    kep.epoch = 1767225600.0 - 0.0005;
    CHECK(writable(kep).line1.substr(18, 14) == "25365.99999999");

    // The counters keep their last digits, and the catalogue number is 99999 at most, as before
    kep = metop_elements();
    kep.element_number = 12345;
    kep.revolutions_at_epoch = 1234567;
    kep.satellite_number = 123456;
    satdump::TLE tle = writable(kep);
    CHECK(tle.line1.substr(0, 8) == "1 99999U");
    CHECK(tle.line1.substr(64, 4) == "2345");
    CHECK(tle.line2.substr(0, 7) == "2 99999");
    CHECK(tle.line2.substr(63, 5) == "34567");
    CHECK(tle.norad == 123456);

    // 0 is the smallest of each
    kep = metop_elements();
    kep.satellite_number = 0;
    kep.element_number = 0;
    kep.revolutions_at_epoch = 0;
    tle = writable(kep);
    CHECK(tle.line1.substr(0, 8) == "1 00000U");
    CHECK(tle.line1.substr(64, 4) == "   0");
    CHECK(tle.line2.substr(63, 5) == "    0");

    // Below 0 each is refused. Before, a short one was written as it was (the catalogue number -5
    // as "-0005"), an element set number of -10000 as 0 (its last 4 digits), and only one too
    // long for its columns was refused
    for (int n : {-1, -5, -10000, -123456})
    {
        CAPTURE(n);
        kep = metop_elements();
        kep.satellite_number = n;
        refused(kep, "catalogue number");
        kep = metop_elements();
        kep.element_number = n;
        refused(kep, "element set number");
        kep = metop_elements();
        kep.revolutions_at_epoch = n;
        refused(kep, "revolution number");
    }

    // TLE text can give them, as the orbit library reads "-0005" as -5; such a set cannot be read
    const std::string line1 = "1 38771U 12049A   25314.50000000  .00000100  00000-0  50000-4 0  999";
    const std::string line2 = "2 38771  98.6900  10.1234 0001500  90.1000 270.0500 14.21512345 6789";
    auto with = [](std::string line, size_t column, const std::string &text) { return line.replace(column, text.size(), text); };
    const std::vector<std::array<std::string, 2>> negative = {
        {with(line1, 2, "-0005"), with(line2, 2, "-0005")}, // catalogue number
        {with(line1, 64, "-999"), line2},                   // element set number
        {line1, with(line2, 63, "-6789")},                  // revolution number
    };
    for (auto &lines : negative)
    {
        std::string text = "METOP-B\n" + tle_line(lines[0]) + "\n" + tle_line(lines[1]) + "\n";
        CAPTURE(text);
        int bad = -1;
        CHECK(satdump::parseKeplerText(text, bad).empty());
        CHECK(bad == 1);
    }
}
