// Fallbacks that used to be silent (an empty catch) now log what happened, and still do
// the same thing as before.
#include "db/db_handler.h"
#include "db/iers/iers_handler.h"
#include "doctest.h"
#include "image/image.h"
#include "image/io.h"
#include "image/meta.h"
#include "log_capture.h"
#include "nlohmann/json.hpp"
#include "pipeline/modules/demod/doppler_qth.h"
#include <filesystem>
#include <memory>
#include <string>
#include <unistd.h>

using satdump::image::Image;
using test_log::LogCapture;

namespace
{
    // Saves a 4x3 8-bit picture with this proj_cfg as a TIFF, checks that the pixels read back
    // the same, and returns whether a warning about GeoTIFF tags naming the file was logged
    bool save_tiff_with_proj_cfg(const nlohmann::json &proj_cfg)
    {
        Image img(8, 4, 3, 1);
        for (int i = 0; i < 12; i++)
            img.set(0, i, i * 20);
        nlohmann::json meta;
        meta["proj_cfg"] = proj_cfg;
        satdump::image::set_metadata(img, meta);

        const std::filesystem::path f = std::filesystem::temp_directory_path() / ("satdump-tiff-" + std::to_string(::getpid()) + ".tif");
        bool warned;
        {
            LogCapture c;
            satdump::image::save_tiff(img, f.string());
            warned = c.has(slog::LOG_WARN, "GeoTIFF") && c.has(slog::LOG_WARN, f.string());
        }

        // The picture itself is written as before
        Image back;
        satdump::image::load_tiff(back, f.string());
        CHECK(back.width() == 4);
        CHECK(back.height() == 3);
        CHECK(back.channels() == 1);
        for (int i = 0; i < 12; i++)
            CHECK(back.get(0, i) == i * 20);
        std::filesystem::remove(f);
        return warned;
    }
} // namespace

TEST_CASE("TIFF: a picture whose projection cannot be read is saved without GeoTIFF tags, with a warning")
{
    // A "type" that is not text: the projection cannot be read
    CHECK(save_tiff_with_proj_cfg({{"type", 5}}));
}

TEST_CASE("TIFF: a picture with no projection is saved without GeoTIFF tags and no warning")
{
    // No projection at all (proj_cfg null, as a picture whose projection was removed has)
    CHECK_FALSE(save_tiff_with_proj_cfg(nullptr));
    // An empty proj_cfg, and the one a crop of a picture with no projection gets
    CHECK_FALSE(save_tiff_with_proj_cfg(nlohmann::json::object()));
    CHECK_FALSE(save_tiff_with_proj_cfg({{"width", 4}, {"height", 3}, {"transform2", {{"x", 0}}}}));
}

TEST_CASE("IERS: entries that cannot be read are skipped as before, and counted in one warning")
{
    auto db = std::make_shared<satdump::DBHandler>(":memory:");
    REQUIRE_FALSE(db->run_sql("CREATE TABLE IF NOT EXISTS iers(time INT PRIMARY KEY NOT NULL, a_pole_x FLOAT NOT NULL, a_pole_y FLOAT NOT NULL, ut1_utc FLOAT NOT NULL);"));
    satdump::IersDBHandler iers(db);

    // MJD 60001 has text that is not a number; 60003 and 60004 have no values yet (rows for
    // dates past the predictions: empty text, or no values at all)
    const std::string bulletin = R"({"EOP": {"data": {"timeSeries": [
        {"time": {"MJD": "60000"}, "dataEOP": {"pole": [{"source": "BulletinA", "X": "0.1", "Y": "0.2"}], "UT": [{"UT1-UTC": "-0.05"}]}},
        {"time": {"MJD": "60001"}, "dataEOP": {"pole": [{"source": "BulletinA", "X": "n/a", "Y": "0.2"}], "UT": [{"UT1-UTC": "-0.05"}]}},
        {"time": {"MJD": "60002"}, "dataEOP": {"pole": [{"source": "BulletinA", "X": "0.3", "Y": "0.4"}], "UT": [{"UT1-UTC": "-0.06"}]}},
        {"time": {"MJD": "60003"}, "dataEOP": {"pole": [{"source": "BulletinA", "X": "", "Y": " "}], "UT": [{"UT1-UTC": ""}]}},
        {"time": {"MJD": "60004"}, "dataEOP": {"pole": [{"source": "BulletinA"}], "UT": [{}]}}
    ]}}})";

    {
        LogCapture c;
        CHECK(iers.storeBulletinA(bulletin));
        CHECK(c.has(slog::LOG_WARN, "1 of 5 entries could not be read"));
        CHECK(c.has(slog::LOG_WARN, "MJD 60001"));
        CHECK(c.has(slog::LOG_DEBUG, "2 of 5 entries have no values yet"));
        CHECK_FALSE(c.has(slog::LOG_WARN, "60003"));
    }
    CHECK(db->get_table_size("iers") == 2);

    // A normal update, where only the rows past the predictions are empty: no warning
    const std::string normal = R"({"EOP": {"data": {"timeSeries": [
        {"time": {"MJD": "60005"}, "dataEOP": {"pole": [{"source": "BulletinA", "X": "0.1", "Y": "0.2"}], "UT": [{"UT1-UTC": "-0.05"}]}},
        {"time": {"MJD": "60006"}, "dataEOP": {"pole": [{"source": "BulletinA", "X": "", "Y": ""}], "UT": [{"UT1-UTC": ""}]}}
    ]}}})";
    {
        LogCapture c;
        CHECK(iers.storeBulletinA(normal));
        CHECK_FALSE(c.has(slog::LOG_WARN, "IERS"));
    }
    CHECK(db->get_table_size("iers") == 3);

    // Text that is not IERS JSON at all is an error, as before
    LogCapture c;
    CHECK_FALSE(iers.storeBulletinA("<html>not json</html>"));
    CHECK(c.has(slog::LOG_ERROR, "Error parsing IERS data"));
    CHECK(db->get_table_size("iers") == 3);
}

namespace
{
    // The "satdump_general" settings with a station position, as satdump_cfg.json holds it
    nlohmann::ordered_json qth_settings(double lon, double lat, double alt)
    {
        nlohmann::ordered_json g;
        g["qth_lat"]["value"] = lat;
        g["qth_lon"]["value"] = lon;
        g["qth_alt"]["value"] = alt;
        return g;
    }
} // namespace

TEST_CASE("Doppler: a station at 0/0 (none set) gets a warning; a real one does not")
{
    using satdump::pipeline::demod::resolve_doppler_qth;

    // The default settings: 0/0/0
    {
        LogCapture c;
        auto q = resolve_doppler_qth(qth_settings(0, 0, 0), nlohmann::json::object());
        CHECK(q.lon == 0);
        CHECK(q.lat == 0);
        CHECK(q.alt == 0);
        CHECK(c.has(slog::LOG_WARN, "station position is 0/0"));
    }

    // A station in the settings
    {
        LogCapture c;
        auto q = resolve_doppler_qth(qth_settings(-117.1, 32.7, 120), nlohmann::json::object());
        CHECK(q.lon == -117.1);
        CHECK(q.lat == 32.7);
        CHECK(q.alt == 120);
        CHECK_FALSE(c.has(slog::LOG_WARN, "Doppler"));
    }

    // Pipeline parameters replace the settings, each on its own
    {
        LogCapture c;
        auto q = resolve_doppler_qth(qth_settings(0, 0, 0), {{"qth_lon", 2.35}, {"qth_lat", 48.85}});
        CHECK(q.lon == 2.35);
        CHECK(q.lat == 48.85);
        CHECK(q.alt == 0);
        CHECK_FALSE(c.has(slog::LOG_WARN, "Doppler"));
    }
    {
        LogCapture c;
        auto q = resolve_doppler_qth(qth_settings(-117.1, 32.7, 120), {{"qth_alt", 5}});
        CHECK(q.lon == -117.1);
        CHECK(q.lat == 32.7);
        CHECK(q.alt == 5);
        CHECK_FALSE(c.has(slog::LOG_WARN, "Doppler"));
    }
}

TEST_CASE("Doppler: a settings value that cannot be read is 0, with a warning naming it")
{
    using satdump::pipeline::demod::resolve_doppler_qth;

    nlohmann::ordered_json g = qth_settings(-117.1, 32.7, 120);
    g["qth_lat"]["value"] = "north";
    g.erase("qth_alt");

    LogCapture c;
    auto q = resolve_doppler_qth(g, nlohmann::json::object());
    CHECK(q.lon == -117.1);
    CHECK(q.lat == 0);
    CHECK(q.alt == 0);
    CHECK(c.has(slog::LOG_WARN, "qth_lat could not be read"));
    CHECK(c.has(slog::LOG_WARN, "qth_alt could not be read"));
    CHECK_FALSE(c.has(slog::LOG_WARN, "qth_lon"));
    CHECK_FALSE(c.has(slog::LOG_WARN, "0/0"));

    // A parameter for the unreadable value: no warning about it
    LogCapture c2;
    q = resolve_doppler_qth(g, {{"qth_lat", 32.7}, {"qth_alt", 120}});
    CHECK(q.lat == 32.7);
    CHECK(q.alt == 120);
    CHECK_FALSE(c2.has(slog::LOG_WARN, "Doppler"));
}
