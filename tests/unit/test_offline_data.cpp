// Fork: orbit elements and Earth orientation (IERS) data from local files (file:// URLs), as the
// Controller supplies them so SatDump never has to go online. Covers: TLE text and OMM CSV both
// read; a local file read at start-up, before init() returns; a missing file reported once at
// once (no ten retries); a damaged or empty file changing nothing; the leap second list in the
// layouts it is published in; the IERS entry nearest in time being the one used; JSON that is not
// a bulletin; an address setting that cannot be read never leading to a download. The IERS data
// is a short copy of the published files (tests/offline); the orbit elements are made up.

#include "db/iers/iers_handler.h"
#include "db/kepler/kepler_handler.h"
#include "doctest.h"
#include "libs/predict/predict.h"
#include "log_capture.h"
#include "offline_rig.h"
#include "utils/file_url.h"
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sqlite3.h>
#include <string>
#include <unistd.h>

using test_log::LogCapture;

namespace
{
    namespace fs = std::filesystem;

    // Made-up elements in the shape of three weather satellites', epoch 2025-11-10 12:00 UTC
    const char *TLE_TEXT = "METOP-B\n"
                           "1 38771U 12049A   25314.50000000  .00000100  00000-0  50000-4 0  9992\n"
                           "2 38771  98.6900  10.1234 0001500  90.1000 270.0500 14.21512345678909\n"
                           "NOAA 19\n"
                           "1 33591U 09005A   25314.50000000  .00000100  00000-0  50000-4 0  9995\n"
                           "2 33591  99.0500  40.5000 0001400 100.2000 260.0000 14.13000000865436\n"
                           "METEOR-M2 3\n"
                           "1 57166U 23091A   25314.50000000  .00000100  00000-0  50000-4 0  9990\n"
                           "2 57166  98.6500  20.7500 0003000  80.5000 279.6000 14.24000000123455\n";
    const double EPOCH = 1762776000; // 2025-11-10 12:00 UTC

    // A folder of the test's own, removed with everything in it at the end
    struct TempDir
    {
        fs::path path;
        TempDir()
        {
            std::string tmpl = (fs::temp_directory_path() / "satdump-offline-XXXXXX").string();
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
            return (path / name).string();
        }
        std::string url(const std::string &name) const { return "file://" + (path / name).string(); }
    };

    std::string fixture(const std::string &name)
    {
        std::ifstream f(std::string(SATDUMP_TEST_SOURCE_DIR) + "/tests/offline/" + name, std::ios::binary);
        REQUIRE(f.good());
        return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }

    size_t warnings(const LogCapture &c)
    {
        size_t n = 0;
        for (auto &m : c.sink->msgs)
            if (m.lvl == slog::LOG_WARN)
                n++;
        return n;
    }

    // The text with one character of a line changed, so its checksum no longer matches
    std::string damaged(std::string text, const std::string &line_start)
    {
        size_t at = text.find(line_start);
        REQUIRE(at != std::string::npos);
        text[at + 30] = text[at + 30] == '5' ? '6' : '5';
        return text;
    }

    // A TLE line from its first 68 characters, with its checksum added
    std::string tle_line(const std::string &first68)
    {
        REQUIRE(first68.size() == 68);
        int sum = 0;
        for (char ch : first68)
            sum += ch == '-' ? 1 : (ch >= '0' && ch <= '9' ? ch - '0' : 0);
        return first68 + char('0' + sum % 10);
    }

    // OMM CSV as the default sources send it: the header line, then METOP-B and NOAA 19 (the
    // columns are those of the header)
    const std::string OMM_HEADER = "OBJECT_NAME,OBJECT_ID,EPOCH,MEAN_MOTION,ECCENTRICITY,INCLINATION,RA_OF_ASC_NODE,ARG_OF_PERICENTER,MEAN_ANOMALY,"
                                   "EPHEMERIS_TYPE,CLASSIFICATION_TYPE,NORAD_CAT_ID,ELEMENT_SET_NO,REV_AT_EPOCH,BSTAR,MEAN_MOTION_DOT,MEAN_MOTION_DDOT\n";
    const std::string OMM_METOP = "METOP-B,2012-049A,2025-11-10T12:00:00.000000,14.21512345,.00015,98.69,10.1234,90.1,270.05,0,U,38771,999,67890,.00005,.000001,0";
    const std::string OMM_NOAA = "NOAA 19,2009-005A,2025-11-10T12:00:00.000000,14.13,.00014,99.05,40.5,100.2,260,0,U,33591,999,86543,.00005,.000001,0";

    // An OMM CSV line with one column replaced
    std::string omm_with(const std::string &line, size_t column, const std::string &value)
    {
        size_t start = 0;
        for (size_t i = 0; i < column; i++)
            start = line.find(',', start) + 1;
        size_t end = line.find(',', start);
        return line.substr(0, start) + value + (end == std::string::npos ? "" : line.substr(end));
    }
} // namespace

TEST_CASE("Orbit elements: TLE text and OMM CSV are both read")
{
    int bad = -1;
    std::vector<satdump::KeplerData> keps = satdump::parseKeplerText(TLE_TEXT, bad);
    CHECK(bad == 0);
    REQUIRE(keps.size() == 3);
    CHECK(keps[0].satellite_number == 38771);
    CHECK(keps[0].name == "METOP-B");
    CHECK(keps[0].epoch == doctest::Approx(EPOCH));
    CHECK(keps[0].mean_motion == doctest::Approx(14.21512345));
    CHECK(keps[2].satellite_number == 57166);
    CHECK(keps[2].name == "METEOR-M2 3");

    // Windows line ends, a "0 " before the name (CelesTrak's 3LE) and a set with no name line
    std::string text = TLE_TEXT;
    std::string crlf;
    for (char c : text)
        crlf += c == '\n' ? std::string("\r\n") : std::string(1, c);
    crlf.replace(0, 7, "0 METOP-B");
    crlf.erase(crlf.find("NOAA 19\r\n"), 9);
    keps = satdump::parseKeplerText(crlf, bad);
    CHECK(bad == 0);
    REQUIRE(keps.size() == 3);
    CHECK(keps[0].name == "METOP-B");
    CHECK(keps[1].satellite_number == 33591);
    CHECK(keps[1].name == "33591");

    // OMM CSV, with its header line, as the default sources send it
    const std::string omm = "OBJECT_NAME,OBJECT_ID,EPOCH,MEAN_MOTION,ECCENTRICITY,INCLINATION,RA_OF_ASC_NODE,ARG_OF_PERICENTER,MEAN_ANOMALY,EPHEMERIS_TYPE,"
                            "CLASSIFICATION_TYPE,NORAD_CAT_ID,ELEMENT_SET_NO,REV_AT_EPOCH,BSTAR,MEAN_MOTION_DOT,MEAN_MOTION_DDOT\n"
                            "METOP-B,2012-049A,2025-11-10T12:00:00.000000,14.21512345,.00015,98.69,10.1234,90.1,270.05,0,U,38771,999,67890,.00005,.000001,0\n";
    keps = satdump::parseKeplerText(omm, bad);
    CHECK(bad == 0);
    REQUIRE(keps.size() == 1);
    CHECK(keps[0].satellite_number == 38771);
    CHECK(keps[0].epoch == doctest::Approx(EPOCH));

    // An OMM line whose epoch cannot be read is not used (its epoch was left unset)
    std::string bad_epoch = omm;
    bad_epoch.replace(bad_epoch.find("2025-11-10T"), 11, "not-a-date ");
    keps = satdump::parseKeplerText(bad_epoch, bad);
    CHECK(bad == 1);
    CHECK(keps.empty());

    // A TLE line with a wrong checksum, and a name left with no lines after it, are counted
    keps = satdump::parseKeplerText(damaged(TLE_TEXT, "2 33591"), bad);
    CHECK(bad == 1);
    CHECK(keps.size() == 2);
    keps = satdump::parseKeplerText(std::string(TLE_TEXT) + "NOAA 21\n", bad);
    CHECK(bad == 1);
    CHECK(keps.size() == 3);

    // A stray line 2 (NOAA 19's name and line 1 lost) is an entry that cannot be read, not the name
    // of the next satellite (here METEOR-M2 3 without its name line); the same for the header of
    // OMM CSV in front of TLE text
    const std::string tle = TLE_TEXT;
    keps = satdump::parseKeplerText(tle.substr(0, tle.find("NOAA 19")) + tle.substr(tle.find("2 33591"), 70) + tle.substr(tle.find("1 57166")), bad);
    CHECK(bad == 1);
    REQUIRE(keps.size() == 2);
    CHECK(keps[1].satellite_number == 57166);
    CHECK(keps[1].name == "57166");
    keps = satdump::parseKeplerText(omm.substr(0, omm.find('\n') + 1) + tle, bad);
    CHECK(bad == 1);
    CHECK(keps.size() == 3);
}

TEST_CASE("Orbit elements: the TLE given out for elements read from TLE text is plain text")
{
    TempDir dir;
    dir.write("elements.tle", TLE_TEXT);
    test_rig::OfflineRig rig(test_rig::general("4 hours", false), test_rig::kepler_sources({dir.url("elements.tle")}));
    std::optional<satdump::TLE> tle = rig.keplers->get_from_norad(38771);
    REQUIRE(tle.has_value());

    // TLE text gives the designator as "12049A": it is padded to 8 characters with spaces, as in a
    // TLE (it was padded with NUL characters, which products then carried)
    CHECK(tle->line1.find('\0') == std::string::npos);
    CHECK(tle->line2.find('\0') == std::string::npos);
    CHECK(tle->line1.substr(9, 8) == "12049A  ");
    predict_orbital_elements_t *elements = predict_parse_tle(tle->line1.c_str(), tle->line2.c_str());
    REQUIRE(elements != nullptr);
    CHECK(elements->satellite_number == 38771);
    predict_destroy_orbital_elements(elements);
}

TEST_CASE("Orbit elements: a designator that fills all 8 columns is read as it is")
{
    // A debris piece with a three-letter designator. The orbit library copied the 8 characters
    // with no NUL after them, and the designator SatDump stored read on into the bytes that follow
    const std::string line1 = tle_line("1 31141U 99025BGQ 25314.50000000  .00000100  00000-0  50000-4 0  999");
    const std::string line2 = tle_line("2 31141  98.6900  10.1234 0001500  90.1000 270.0500 14.2151234567890");

    // The memory the library is about to get is filled first, so bytes left unset show: blocks of
    // its size are filled and freed, and the allocator hands the last ones freed out first. A few,
    // as the reading below takes one or two for its own text first. (Called through volatile
    // pointers, so the compiler cannot drop them)
    const auto fill_freed_memory = []
    {
        void *(*volatile allocate)(size_t) = std::malloc;
        void (*volatile release)(void *) = std::free;
        std::vector<void *> blocks;
        for (int i = 0; i < 8; i++)
            if (void *block = allocate(sizeof(predict_orbital_elements_t)))
                blocks.push_back(std::memset(block, 0xA5, sizeof(predict_orbital_elements_t)));
        for (void *block : blocks)
            release(block);
        return blocks.size();
    };
    REQUIRE(fill_freed_memory() == 8);
    predict_orbital_elements_t *elements = predict_parse_tle(line1.c_str(), line2.c_str());
    REQUIRE(elements != nullptr);
    CHECK(strnlen(elements->designator, sizeof(elements->designator)) == 8);
    predict_destroy_orbital_elements(elements);

    // SatDump's copy stops at 8 characters on its own, even if the library left no NUL
    const std::string text = "DEBRIS\n" + line1 + "\n" + line2 + "\n";
    int bad = -1;
    REQUIRE(fill_freed_memory() == 8);
    std::vector<satdump::KeplerData> keps = satdump::parseKeplerText(text, bad);
    CHECK(bad == 0);
    REQUIRE(keps.size() == 1);
    CHECK(keps[0].designator == "99025BGQ");
    CHECK(satdump::keplerToTle(keps[0]).line1.substr(9, 8) == "99025BGQ");
}

TEST_CASE("Orbit elements: a designator with a quote is stored, with the rest of its file")
{
    // It went into the database's SQL as it was, so that entry failed while the rest of the file
    // was stored
    TempDir dir;
    dir.write("elements.csv", OMM_HEADER + OMM_METOP + "\n" + omm_with(OMM_NOAA, 1, "2009-005'A") + "\n");
    LogCapture c;
    test_rig::OfflineRig rig(test_rig::general("4 hours", false), test_rig::kepler_sources({dir.url("elements.csv")}));
    satdump::KeplerData kep;
    REQUIRE(rig.keplers->getKepler(kep, 33591));
    CHECK(kep.designator == "2009-005'A");
    CHECK(rig.keplers->getAllNewestKepler().size() == 2);
    CHECK_FALSE(c.has(slog::LOG_ERROR, "Error inserting"));
}

TEST_CASE("Orbit elements: a local file is read at start-up, before init() returns")
{
    TempDir dir;
    dir.write("elements.tle", TLE_TEXT);

    LogCapture c;
    test_rig::OfflineRig rig(test_rig::general("4 hours", false), test_rig::kepler_sources({dir.url("elements.tle")}));
    // Read by init() itself: the update used to wait for the task thread, after start-up
    satdump::KeplerData kep;
    CHECK(rig.keplers->getKepler(kep, 38771));
    CHECK(rig.keplers->getAllNewestKepler().size() == 3);
    CHECK(rig.keplers->all_.size() == 3);
    CHECK(c.has(slog::LOG_INFO, "Read 3 orbit elements from file://"));
    CHECK(warnings(c) == 0);
}

TEST_CASE("Orbit elements: which settings count as local files only")
{
    TempDir dir;
    test_rig::OfflineRig rig(test_rig::general("Never", false));
    auto local_only = [](const std::vector<std::string> &urls, const std::vector<int> &norads)
    {
        satdump::satdump_cfg.main_cfg["kepler_settings"] = test_rig::kepler_sources(urls, norads);
        return satdump::KeplerDBHandler::allSourcesAreLocal();
    };
    CHECK(local_only({dir.url("a.tle")}, {}));
    CHECK(local_only({dir.url("a.tle"), dir.url("b.tle")}, {}));
    CHECK_FALSE(local_only({}, {}));
    CHECK_FALSE(local_only({dir.url("a.tle"), "https://example.invalid/gp.php"}, {}));
    CHECK_FALSE(local_only({dir.url("a.tle")}, {38771}));

    // Settings that are missing or not lists count as no sources, with a warning (no exception)
    satdump::satdump_cfg.main_cfg.erase("kepler_settings");
    CHECK_FALSE(satdump::KeplerDBHandler::allSourcesAreLocal());
    LogCapture c;
    satdump::satdump_cfg.main_cfg["kepler_settings"]["urls_to_fetch"] = "not a list";
    CHECK_FALSE(satdump::KeplerDBHandler::allSourcesAreLocal());
    CHECK(c.has(slog::LOG_WARN, "Cannot read the orbit element sources"));
}

TEST_CASE("Orbit elements: a missing, damaged or empty file changes nothing, with one warning")
{
    TempDir dir;
    std::string path = dir.write("elements.tle", TLE_TEXT);
    test_rig::OfflineRig rig(test_rig::general("4 hours", false), test_rig::kepler_sources({dir.url("elements.tle")}));
    REQUIRE(rig.keplers->getAllNewestKepler().size() == 3);

    auto unchanged = [&rig]()
    {
        satdump::KeplerData kep;
        return rig.keplers->getAllNewestKepler().size() == 3 && rig.keplers->all_.size() == 3 && rig.keplers->getKepler(kep, 33591) && kep.mean_anomaly == doctest::Approx(260.0);
    };

    SUBCASE("missing: reported once, at once (it was tried ten times, a second apart)")
    {
        fs::remove(path);
        LogCapture c;
        auto start = std::chrono::steady_clock::now();
        rig.keplers->updateKeplerDatabase();
        CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
        CHECK(warnings(c) == 1);
        CHECK(c.has(slog::LOG_WARN, "Cannot read the orbit element file"));
        CHECK(c.has(slog::LOG_WARN, "no such file"));
        CHECK_FALSE(c.has(slog::LOG_TRACE, "Retrying"));
        CHECK(unchanged());
    }

    SUBCASE("a folder where the file should be")
    {
        fs::remove(path);
        fs::create_directory(path);
        LogCapture c;
        rig.keplers->updateKeplerDatabase();
        CHECK(warnings(c) == 1);
        CHECK(c.has(slog::LOG_WARN, "it is a folder"));
        CHECK(unchanged());
    }

    SUBCASE("damaged: one bad entry means none of the file is used")
    {
        // NOAA 19's mean anomaly changed, and METOP-B's line 2 broken
        std::string text = TLE_TEXT;
        text.replace(text.find("260.0000 14.13000000865436"), 26, "261.0000 14.13000000865437");
        dir.write("elements.tle", damaged(text, "2 38771"));
        LogCapture c;
        rig.keplers->updateKeplerDatabase();
        CHECK(warnings(c) == 1);
        CHECK(c.has(slog::LOG_WARN, "1 entries that cannot be read"));
        CHECK(unchanged());
    }

    SUBCASE("cut short while being written")
    {
        std::string text = TLE_TEXT;
        dir.write("elements.tle", text.substr(0, text.size() - 40));
        LogCapture c;
        rig.keplers->updateKeplerDatabase();
        CHECK(warnings(c) == 1);
        CHECK(unchanged());
    }

    SUBCASE("empty: the elements already loaded are kept")
    {
        dir.write("elements.tle", "");
        LogCapture c;
        rig.keplers->updateKeplerDatabase();
        CHECK(warnings(c) == 1);
        CHECK(c.has(slog::LOG_WARN, "holds no elements"));
        CHECK(unchanged());
    }

    SUBCASE("a newer file replaces nothing it does not hold, and adds what it does")
    {
        std::string text = TLE_TEXT;
        text.replace(text.find("25314.50000000  .00000100  00000-0  50000-4 0  9995"), 51, "25315.50000000  .00000100  00000-0  50000-4 0  9996");
        dir.write("elements.tle", text);
        LogCapture c;
        rig.keplers->updateKeplerDatabase();
        CHECK(warnings(c) == 0);
        satdump::KeplerData kep;
        REQUIRE(rig.keplers->getKepler(kep, 33591, EPOCH + 86400));
        CHECK(kep.epoch == doctest::Approx(EPOCH + 86400));
        CHECK(rig.keplers->getAllNewestKepler().size() == 3);
    }
}

TEST_CASE("file:// URLs: the forms read, and the reasons given when a file cannot be read")
{
    TempDir dir;
    std::string path = dir.write("data.txt", "hello");
    std::string content, error;
    CHECK(satdump::isFileUrl("file:///x"));
    CHECK_FALSE(satdump::isFileUrl("https://example.invalid/x"));

    CHECK(satdump::readFileUrl("file://" + path, content, error));
    CHECK(content == "hello");
    content.clear();
    CHECK(satdump::readFileUrl("file://localhost" + path, content, error));
    CHECK(content == "hello");

    CHECK_FALSE(satdump::readFileUrl("file://otherhost" + path, content, error));
    CHECK(error.find("full path") != std::string::npos);
    CHECK_FALSE(satdump::readFileUrl("file://" + (dir.path / "none.txt").string(), content, error));
    CHECK(error == "no such file");
    CHECK_FALSE(satdump::readFileUrl("file://" + dir.path.string(), content, error));
    CHECK(error == "it is a folder");
    CHECK_FALSE(satdump::readFileUrl("https://example.invalid/x", content, error));

    if (geteuid() != 0) // root reads any file
    {
        fs::permissions(path, fs::perms::none);
        CHECK_FALSE(satdump::readFileUrl("file://" + path, content, error));
        CHECK(error.find("cannot be opened") != std::string::npos);
        fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write);
    }
}

TEST_CASE("IERS: the bulletin and the leap second list are stored, and the entry nearest in time is used")
{
    test_rig::OfflineRig rig(test_rig::general("Never", false));

    // No data yet: all zero (the values were left unset)
    satdump::IERSInfo none = rig.iers->getBestIERSInfo(EPOCH);
    CHECK(none.ut1_utc == 0);
    CHECK(none.polar_dx == 0);
    CHECK(none.polar_dy == 0);
    CHECK(none.leap_seconds == 0);

    LogCapture c;
    CHECK(rig.iers->storeBulletinA(fixture("finals2000A.daily.json")));
    CHECK(rig.iers->storeLeapSeconds(fixture("leap-seconds.list")));
    CHECK(warnings(c) == 0);
    CHECK(rig.db->get_table_size("iers") == 4);
    CHECK(rig.db->get_table_size("leap_seconds") == 28);

    // Each time gets the entry for its own day. The query sorted by a constant, so every time
    // got the first entry stored (MJD 61266)
    satdump::IERSInfo nov = rig.iers->getBestIERSInfo(1794268800 + 600); // 2026-11-10, MJD 61354
    CHECK(std::fabs(nov.ut1_utc - -0.0652361) < 2e-6);
    CHECK(std::fabs(nov.polar_dx - 126.001) < 2e-3); // arc seconds stored, milli-arc seconds given
    CHECK(nov.leap_seconds == 37);
    satdump::IERSInfo feb = rig.iers->getBestIERSInfo(1802217600 - 600); // 2027-02-10, a prediction
    CHECK(std::fabs(feb.ut1_utc - -0.1533475) < 2e-6);
    satdump::IERSInfo aug = rig.iers->getBestIERSInfo(1786665600); // 2026-08-14, MJD 61266
    CHECK(std::fabs(aug.ut1_utc - 0.0103885) < 2e-6);
    CHECK(rig.iers->getBestIERSInfo(644198400).leap_seconds == 25); // 1990-06-01
}

TEST_CASE("IERS: the leap second list in both published layouts; a damaged one changes nothing")
{
    test_rig::OfflineRig rig(test_rig::general("Never", false));

    // The copy in tzdata (IANA) has tabs between the columns
    CHECK(rig.iers->storeLeapSeconds("#\tleap seconds\n2272060800\t10\t# 1 Jan 1972\n3692217600\t37\t# 1 Jan 2017\n"));
    CHECK(rig.db->get_table_size("leap_seconds") == 2);
    CHECK(rig.iers->getBestIERSInfo(EPOCH).leap_seconds == 37);

    LogCapture c;
    CHECK_FALSE(rig.iers->storeLeapSeconds("2272060800 10 # 1 Jan 1972\n3692217600 thirty-seven\n"));
    CHECK(warnings(c) == 1);
    CHECK(c.has(slog::LOG_WARN, "line 2"));
    CHECK_FALSE(rig.iers->storeLeapSeconds("2272060800 10 # 1 Jan 1972\n3692217600 37 1 Jan 2017\n"));
    CHECK_FALSE(rig.iers->storeLeapSeconds("<html><body>Not found</body></html>\n"));
    CHECK_FALSE(rig.iers->storeLeapSeconds("# only comments\n\n"));
    CHECK(rig.db->get_table_size("leap_seconds") == 2);
    CHECK(rig.iers->getBestIERSInfo(EPOCH).leap_seconds == 37);

    // IERS JSON with no entries is not an update either
    CHECK_FALSE(rig.iers->storeBulletinA("{}"));
    CHECK_FALSE(rig.iers->storeBulletinA(R"({"EOP": {"data": {"timeSeries": []}}})"));
    CHECK(c.has(slog::LOG_WARN, "no entries that can be used"));
    CHECK(rig.db->get_table_size("iers") == 0);
}

TEST_CASE("IERS: JSON that is not a bulletin changes nothing and leaves no transaction open")
{
    test_rig::OfflineRig rig(test_rig::general("Never", false));
    REQUIRE(rig.iers->storeBulletinA(fixture("finals2000A.daily.json")));

    // Each threw inside the database transaction, which was then left open (so storing the leap
    // seconds next failed with "cannot start a transaction within a transaction")
    LogCapture c;
    for (const char *text : {"[]", R"({"EOP": "x"})", R"({"EOP": {"data": {"timeSeries": {}}}})", R"({"EOP": {"data": {"timeSeries": [5, "x", null, []]}}})"})
    {
        CAPTURE(text);
        CHECK_FALSE(rig.iers->storeBulletinA(text));
        CHECK(sqlite3_get_autocommit(rig.db->db) != 0);
    }
    CHECK(rig.db->get_table_size("iers") == 4);
    CHECK(rig.iers->storeLeapSeconds(fixture("leap-seconds.list")));
    CHECK_FALSE(c.has(slog::LOG_ERROR, "transaction"));
}

TEST_CASE("IERS: local files are read at start-up; a missing one gives one warning")
{
    TempDir dir;
    dir.write("finals.json", fixture("finals2000A.daily.json"));
    dir.write("leap.list", fixture("leap-seconds.list"));

    {
        LogCapture c;
        test_rig::OfflineRig rig(test_rig::general("Never", true, dir.url("finals.json"), dir.url("leap.list")));
        CHECK(rig.db->get_table_size("iers") == 4);
        CHECK(rig.db->get_table_size("leap_seconds") == 28);
        CHECK(rig.db->get_meta("iers_last_updated", "") != "");
        CHECK(warnings(c) == 0);
        CHECK(c.has(slog::LOG_INFO, "Reading the IERS bulletin from file://"));
    }

    {
        // The leap second file is missing: the bulletin is still stored, the warning names the file
        LogCapture c;
        test_rig::OfflineRig rig(test_rig::general("Never", true, dir.url("finals.json"), dir.url("none.list")));
        CHECK(rig.db->get_table_size("iers") == 4);
        CHECK(rig.db->get_table_size("leap_seconds") == 0);
        CHECK(warnings(c) == 1);
        CHECK(c.has(slog::LOG_WARN, "Cannot read the leap second list file file://"));
        CHECK(rig.db->get_meta("iers_last_updated", "") == "");
    }

    {
        // Updates off: the files are not read
        test_rig::OfflineRig rig(test_rig::general("Never", false, dir.url("finals.json"), dir.url("leap.list")));
        CHECK(rig.db->get_table_size("iers") == 0);
    }

    {
        // An address not in the form {"value": "<address>"} (here bare text naming a web address)
        // is not set: that file is not updated, with one warning, and nothing is downloaded. The
        // other file, local, is still read at start-up
        LogCapture c;
        nlohmann::ordered_json g = test_rig::general("Never", true, "", dir.url("leap.list"));
        g["iers_bulletin_url"] = "https://example.invalid/finals2000A.all.json";
        test_rig::OfflineRig rig(g);
        CHECK(satdump::IersDBHandler::bulletinUrl() == "");
        CHECK(rig.db->get_table_size("leap_seconds") == 28);
        CHECK(warnings(c) == 1);
        CHECK(c.has(slog::LOG_WARN, "No address is set for the IERS bulletin"));
        CHECK_FALSE(c.has(slog::LOG_TRACE, "Downloading"));
    }

    // No address in the settings at all: there is no built-in web address to fall back on (it
    // used to be the one SatDump always used), so nothing is fetched, with one warning per file
    LogCapture c;
    test_rig::OfflineRig rig(test_rig::general("Never", true));
    CHECK(satdump::IersDBHandler::bulletinUrl() == "");
    CHECK(satdump::IersDBHandler::leapSecondsUrl() == "");
    CHECK(warnings(c) == 2);
    CHECK(c.has(slog::LOG_WARN, "No address is set for the leap second list"));
    CHECK_FALSE(c.has(slog::LOG_TRACE, "Downloading"));
}
