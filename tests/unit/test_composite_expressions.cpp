// Composite expressions (muparser). On Linux SatDump uses Debian's libmuparser (2.3.4); before
// the fork it compiled its own copy, a 2.3.3 development snapshot. These tests evaluate the
// composite expressions of the MetOp, NOAA and Meteor instruments (and a few GOES ABI and
// common ones) the way products/image/product_expression.cpp does, and compare every result
// bit for bit with the values the bundled copy gave (recorded before the switch). They also
// check the error behaviour, SatDump's own callbacks (lut, equp) and both expression paths
// (image::generate_image_expression and products::generate_expression_product_composite).
#include "core/exception.h"
#include "doctest.h"
#include "image/expression.h"
#include "image/image.h"
#include "products/image/product_expression.h"
#include "products/image_product.h"
#include "utils/muparser.h"
#include "utils/string.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    // FNV-1a, 64 bit, over everything a result consists of
    struct Hash
    {
        uint64_t h = 1469598103934665603ULL;

        void bytes(const void *p, size_t n)
        {
            const uint8_t *b = (const uint8_t *)p;
            for (size_t i = 0; i < n; i++)
            {
                h ^= b[i];
                h *= 1099511628211ULL;
            }
        }

        void u64(uint64_t v) { bytes(&v, sizeof(v)); }

        // Every NaN counts as the same value (its sign and payload carry no meaning here)
        void dbl(double v)
        {
            uint64_t bits;
            if (std::isnan(v))
                bits = 0x7ff8000000000000ULL;
            else
                std::memcpy(&bits, &v, sizeof(bits));
            u64(bits);
        }

        void str(const std::string &s)
        {
            u64(s.size());
            bytes(s.data(), s.size());
        }
    };

    uint64_t bits_of(double v)
    {
        uint64_t bits;
        std::memcpy(&bits, &v, sizeof(bits));
        return bits;
    }

    // Stand-ins for SatDump's lut() and equp() callbacks, with the same signatures and
    // registered the same way (product_expression.cpp). Each gives a value that depends on all
    // its arguments; equp also depends on the pixel, which is why SatDump registers it with
    // optimisation off (muparser must call it for every pixel, never fold it to a constant).
    struct LutData
    {
        double k;
    };

    double fake_lut(void *userdata, double x, double y, double ch)
    {
        double k = ((LutData *)userdata)->k;
        return std::fmod(std::fabs(x * 0.731 + y * 0.197 + ch * 0.311 + k), 1.0);
    }

    struct EqupData
    {
        double k;
        const int *pixel;
    };

    double fake_equp(void *userdata, double ch)
    {
        EqupData *d = (EqupData *)userdata;
        return std::fmod(ch * 0.37 + d->k + *d->pixel * 0.0123, 1.0);
    }

    // Channel values the variables take: the edges, the thresholds the expressions test
    // against, values out of range, and a spread in between
    const double VALUES[] = {0.0, 1.0, 0.5, 0.065, 0.305, 0.28, 0.01, -0.25, 1.25, 1e-12, 0.999999, 0.000001,
                             0.123, 0.876, 0.333, 0.667, 0.05, 0.95, 0.42, 0.58, 0.2, 0.8, 0.71, 0.29,
                             2.0, -1.0, 0.0625, 0.9375, 0.15, 0.85, 0.47, 0.53};
    const int NVALUES = sizeof(VALUES) / sizeof(VALUES[0]);
    const int NPIXELS = 48;

    // One composite expression from resources/instrument_cfgs, taken apart and evaluated as in
    // generate_expression_product_composite: the set-up part before the last ';' declares
    // calibrated channels, lut() and equp() functions and macros; the tokens the expression
    // needs are found by defining each unknown token in turn (get_required_tokens); then every
    // token becomes a variable and the expression is evaluated for NPIXELS sets of values.
    uint64_t evaluate_composite(std::string expression, std::string &summary)
    {
        Hash hash;

        expression.erase(std::remove(expression.begin(), expression.end(), '\r'), expression.end());
        expression.erase(std::remove(expression.begin(), expression.end(), '\n'), expression.end());
        expression.erase(std::remove(expression.begin(), expression.end(), ' '), expression.end());

        // {unit} macros name a channel; any channel name will do here
        size_t open;
        while ((open = expression.find('{')) != std::string::npos)
        {
            size_t close = expression.find('}', open);
            if (close == std::string::npos)
                break;
            expression.replace(open, close - open + 1, "9");
        }

        std::vector<std::string> luts, equps;
        if (expression.find(';') != std::string::npos)
        {
            auto split_cfg = satdump::splitString(expression, ';');
            expression = split_cfg[split_cfg.size() - 1];
            for (auto &cfg : split_cfg)
            {
                if (cfg == expression)
                    continue;

                // Same order of checks as product_expression.cpp
                std::string parenthesis = cfg.substr(cfg.find_first_of("(") + 1, cfg.find_first_of(")") - cfg.find_first_of("(") - 1);
                auto parts = satdump::splitString(parenthesis, ',');
                if (parts.size() == 4)
                {
                    try
                    {
                        hash.dbl(std::stod(parts[2]));
                        hash.dbl(std::stod(parts[3]));
                    }
                    catch (const std::exception &)
                    {
                        summary = "bad set-up";
                        hash.str(summary);
                        return hash.h;
                    }
                }
                else if (cfg.find("=lut") != std::string::npos)
                    luts.push_back(cfg.substr(0, cfg.find("=lut")));
                else if (cfg.find("=equp") != std::string::npos)
                    equps.push_back(cfg.substr(0, cfg.find("=equp")));
                else if (cfg.find("=") != std::string::npos)
                {
                    auto macro = satdump::splitString(cfg, '=');
                    if (macro.size() == 2 && macro[0] != "ref_channel")
                        satdump::replaceAllStr(expression, macro[0], "(" + macro[1] + ")");
                }
            }
        }
        hash.str(expression);

        std::vector<LutData> lut_data(luts.size());
        std::vector<EqupData> equp_data(equps.size());
        int pixel = 0;
        for (size_t i = 0; i < luts.size(); i++)
            lut_data[i].k = 0.1 * (i + 1);
        for (size_t i = 0; i < equps.size(); i++)
            equp_data[i] = {0.05 * (i + 1), &pixel};

        try
        {
            // The tokens, as get_required_tokens finds them
            std::vector<std::string> tokens;
            int nout = 0;
            {
                mu::Parser p;
                for (size_t i = 0; i < luts.size(); i++)
                    p.DefineFunUserData(luts[i].c_str(), fake_lut, &lut_data[i]);
                for (size_t i = 0; i < equps.size(); i++)
                    p.DefineFunUserData(equps[i].c_str(), fake_equp, &equp_data[i]);
                while (true)
                {
                    try
                    {
                        p.SetExpr(expression);
                        p.Eval(nout);
                    }
                    catch (mu::ParserError &e)
                    {
                        if (e.GetCode() == mu::ecUNASSIGNABLE_TOKEN)
                        {
                            tokens.push_back(e.GetToken());
                            p.DefineConst(e.GetToken(), 1);
                            continue;
                        }
                        throw;
                    }
                    break;
                }
            }
            for (auto &t : tokens)
                hash.str(t);
            hash.u64(nout);

            // The evaluation, as in the main loop (the expression is set before the rest)
            mu::Parser p;
            p.SetExpr(expression);
            for (size_t i = 0; i < luts.size(); i++)
                p.DefineFunUserData(luts[i].c_str(), fake_lut, &lut_data[i]);
            for (size_t i = 0; i < equps.size(); i++)
                p.DefineFunUserData(equps[i].c_str(), fake_equp, &equp_data[i], false);
            std::vector<double> vars(tokens.size());
            for (size_t i = 0; i < tokens.size(); i++)
                p.DefineVar(tokens[i], &vars[i]);

            for (pixel = 0; pixel < NPIXELS; pixel++)
            {
                for (size_t i = 0; i < vars.size(); i++)
                    vars[i] = VALUES[(pixel * 7 + i * 3) % NVALUES];
                int n = 0;
                double *out = p.Eval(n);
                hash.u64(n);
                for (int c = 0; c < n; c++)
                    hash.dbl(out[c]);
            }
            summary = std::to_string(tokens.size()) + " tokens, " + std::to_string(nout) + " outputs";
        }
        catch (mu::ParserError &e)
        {
            summary = "error " + std::to_string(e.GetCode()) + ": " + e.GetMsg();
            hash.u64(e.GetCode());
            hash.str(e.GetMsg());
            hash.u64(e.GetPos());
            hash.str(e.GetToken());
        }
        return hash.h;
    }

    struct Composite
    {
        const char *name;
        const char *expression;
    };

    // Copied from resources/instrument_cfgs (the order in each file; "#n" counts its
    // expressions), so that later edits there do not change what this test checks: every
    // expression of avhrr_3, msu_mr, mhs, amsu_a, hirs, iasi, gome, common/automatic and abi,
    // 119 of the 487 in resources/
    const Composite COMPOSITES[] = {
        {"avhrr_3 #0", R"x(ch2, ch2, ch1)x"},
        {"avhrr_3 #1", R"x(cch3a=(3a,sun_angle_compensated_reflective_radiance, 0,  4.3332); cch2=(2, sun_angle_compensated_reflective_radiance, 0, 77.64); cch1=(1, sun_angle_compensated_reflective_radiance, 0, 36.9624); lut=lut(lut/1d/stretchcon_avhrr.png);lut(cch3a, 0, 0), lut(cch2, 0, 1),lut(cch1, 0, 2))x"},
        {"avhrr_3 #2", R"x(cch3a=(3a, reflective_radiance, 0,  2.5); cch2=(2, reflective_radiance, 0, 50); cch1=(1, reflective_radiance, 0, 23); lut=lut(lut/1d/stretchcon_avhrr.png);lut(cch3a, 0, 0), lut(cch2, 0, 1),lut(cch1, 0, 2))x"},
        {"avhrr_3 #3", R"x(ch1 < 0.065 ? (ch4 - 0.7) * 2.66 : ch2 * 2.2 - 0.15, ch1 < 0.065 ? (ch4 - 0.7) * 2.66 : ch2 * 2.2 - 0.15, ch1 < 0.065 ? (ch4 - 0.7) * 2.66 : ch1 * 2.2 - 0.15)x"},
        {"avhrr_3 #4", R"x(cch2=(2, sun_angle_compensated_reflective_radiance, 0, 97.0507); cch1=(1, sun_angle_compensated_reflective_radiance, 0, 46.203); cch2 * 0.6 + min(cch1, cch2) * 0.4, cch2, cch1)x"},
        {"avhrr_3 #5", R"x(cch3a=(3a, sun_angle_compensated_reflective_radiance, 0, 5.41665); cch2=(2, sun_angle_compensated_reflective_radiance, 0, 97.0507); cch1=(1, sun_angle_compensated_reflective_radiance, 0, 46.203); ((0.5*max(cch3a,min(cch2,cch1)) + 0.5*cch2) / 0.85)^0.65, (cch2/0.85)^0.65, (cch1/0.85)^0.65)x"},
        {"avhrr_3 #6", R"x(ch5, ch4, ch3b)x"},
        {"avhrr_3 #7", R"x(ch3b, ch4, ch5)x"},
        {"avhrr_3 #8", R"x(cch1=(1, sun_angle_compensated_reflective_radiance, 0, 33.3);cch2=(2, sun_angle_compensated_reflective_radiance, 0, 67.84);cch4=(4, brightness_temperature, 203, 323); cch1, cch2, 1-cch4)x"},
        {"avhrr_3 #9", R"x(cch4=(4, brightness_temperature, 203.000000, 323.000000);cch3a=(3a, sun_angle_compensated_reflective_radiance, 0.000000, 3.791655);cch2=(2, sun_angle_compensated_reflective_radiance, 0.000000, 97.050700);cch2,cch3a,cch4)x"},
        {"avhrr_3 #10", R"x(cch1=(1, sun_angle_compensated_reflective_radiance, 0, 46.203); cch4=(4, brightness_temperature, 203, 323); cch1^0.58823529411,cch1^0.58823529411,1-cch4)x"},
        {"avhrr_3 #11", R"x(cch4=(4, brightness_temperature, 304.912379, 232.242185);cch3a=(3a, sun_angle_compensated_reflective_radiance, 0.000000, 5.416650);cch2=(2, sun_angle_compensated_reflective_radiance, 0.000000, 97.050700);cch1=(1, sun_angle_compensated_reflective_radiance, 0.000000, 46.203000);((3*cch1 - cch4) / 2),((3*cch1 + cch4) / 4),(cch1 - cch3a))x"},
        {"avhrr_3 #12", R"x(cch4=(4, brightness_temperature, 0.000000, 1000.000000);cch3b=(3b, brightness_temperature, 0.000000, 1000.000000);cch3b > 0.305 ? cch4 > 0.280 ? (cch3b-cch4) > 0.01 ? 1 : 0 : 0 : 0,0,0,cch3b > 0.305 ? cch4 > 0.280 ? (cch3b-cch4) > 0.01 ? 1 : 0 : 0 : 0)x"},
        {"avhrr_3 #13", R"x(bm=equp(maps/map_aweeri.jpg); chb * (chb) + (1-chb) * bm(0), chb * (chb) + (1-chb) * bm(1), chb * (chb) + (1-chb) * bm(2))x"},
        {"avhrr_3 #14", R"x(bm=equp(maps/map_aweeri.jpg); ch=(cha) * 1.5;ch * (ch) + (1-ch) * bm(0), ch * (ch) + (1-ch) * bm(1), ch * (ch) + (1-ch) * bm(2))x"},
        {"avhrr_3 #15", R"x(bm=equp(maps/map_aweeri.jpg);lut=lut(lut/cal/WXtoImg-NO.png);lut(1-chb, 0, 0) * (chb) + (1-chb) * bm(0), lut(1-chb, 0, 1) * (chb) + (1-chb) * bm(1),lut(1-chb, 0, 2) * (chb) + (1-chb) * bm(2))x"},
        {"msu_mr #0", R"x(cch1=(1, sun_angle_compensated_reflective_radiance, 1.000000, 78);cch2=(2, sun_angle_compensated_reflective_radiance, 1.000000, 100);lut=lut(lut/NDVI.png);lut(0, ((cch2-cch1)/((cch2+cch1) == 0 ? 0.000001 : (cch1+cch2))+1)/2, 0),lut(0, ((cch2-cch1)/((cch2+cch1) == 0 ? 0.000001 : (cch1+cch2))+1)/2, 1),lut(0, ((cch2-cch1)/((cch2+cch1) == 0 ? 0.000001 : (cch1+cch2))+1)/2, 2))x"},
        {"msu_mr #1", R"x(ch2, ch2, ch1)x"},
        {"msu_mr #2", R"x(cch2=(2, reflective_radiance, 0, 25; cch1=(1, reflective_radiance, 0, 15); cch2, cch2, cch1)x"},
        {"msu_mr #3", R"x(cch2=(2, sun_angle_compensated_reflective_radiance, 0, 43.5;cch1=(1, sun_angle_compensated_reflective_radiance, 0, 31.5); lut=lut(lut/1d/stretchcon_avhrr.png);lut(cch2, 0, 0), lut(cch2, 0, 1),lut(cch1, 0, 2))x"},
        {"msu_mr #4", R"x(ch3, ch2, ch1)x"},
        {"msu_mr #5", R"x(cch3=(3,reflective_radiance, 0, 3.465); cch2=(2, reflective_radiance, 0, 19.25); cch1=(1, reflective_radiance, 0, 11.55);lut=lut(lut/1d/stretchcon_avhrr.png);lut(cch3, 0, 0), lut(cch2, 0, 1),lut(cch1, 0, 2))x"},
        {"msu_mr #6", R"x(cch3=(3, sun_angle_compensated_reflective_radiance, 0, 4.95); cch2=(2, sun_angle_compensated_reflective_radiance, 0, 27.5); cch1=(1, sun_angle_compensated_reflective_radiance, 0, 16.5); cch3, cch2, cch1)x"},
        {"msu_mr #7", R"x(cch1=(1, sun_angle_compensated_reflective_radiance, 0, 25.3);cch2=(2, sun_angle_compensated_reflective_radiance, 0, 34.84);cch4=(4, brightness_temperature, 263, 383); cch1, cch2, (1-cch4)/2)x"},
        {"msu_mr #8", R"x(cch1=(1, sun_angle_compensated_reflective_radiance, 0, 25.3);cch2=(2, sun_angle_compensated_reflective_radiance, 0, 34.84);cch4=(5, brightness_temperature, 203, 323); cch1, cch2, 1-cch4)x"},
        {"msu_mr #9", R"x(cch1=(1, reflective_radiance, 0.000000,15.203000);cch2=(2, reflective_radiance, 0.000000, 24.050700);cch3=(3, reflective_radiance, 0.000000, 3.500000);0.6*max(cch3,min(cch2,ch1)) + 0.35*cch2, cch2, cch1)x"},
        {"msu_mr #10", R"x(1 - ch5)x"},
        {"msu_mr #11", R"x(cch3=(3, sun_angle_compensated_reflective_radiance, 0, 6.265); cch2=(2, sun_angle_compensated_reflective_radiance, 0, 36.7); cch5=(5, brightness_temperature, 203.000000, 323.000000);cch2,cch3,cch5)x"},
        {"msu_mr #12", R"x(1 - ch6, 1 - ch5, 1 - ch4)x"},
        {"msu_mr #13", R"x(1 - ch4, 1 - ch5, 1 - ch6)x"},
        {"msu_mr #14", R"x(cch4=(4, brightness_temperature, 0.000000, 1000.000000);cch3b=(3b, brightness_temperature, 0.000000, 1000.000000);cch3b > 0.305 ? cch4 > 0.280 ? (cch3b-cch4) > 0.01 ? 1 : 0 : 0 : 0,0,0,cch3b > 0.305 ? cch4 > 0.280 ? (cch3b-cch4) > 0.01 ? 1 : 0 : 0 : 0)x"},
        {"msu_mr #15", R"x(cch1=(1, sun_angle_compensated_reflective_radiance, 0.9, 20);cch5=(5, brightness_temperature_celsius, -60.000000, 40.000000);lut=lut(lut/2dfc/2dfc_vis.png);lut(cch5,cch1^0.9,0),lut(cch5,cch1^0.9,1),lut(cch5,cch1^0.9,2))x"},
        {"msu_mr #16", R"x(cch2=(2, sun_angle_compensated_reflective_radiance, 0.000000, 95.050700);cch4=(4, brightness_temperature_celsius, -60.000000, 40.000000);lut=lut(lut/2dfc/2dfc_vis.png);lut(cch4,cch2^0.9,0),lut(cch4,cch2^0.9,1),lut(cch4,cch2^0.9,2))x"},
        {"msu_mr #17", R"x(cch4=(4, brightness_temperature, 290.595419, 393.815443);cch2=(2, reflective_radiance, 0, 27.5); cch1=(1, reflective_radiance, 0, 16.5); cch4^0.9, cch2^0.9, cch1^0.9)x"},
        {"mhs #0", R"x(cch2=(2, emissive_radiance, 0.039, 0.068); cch1=(1, emissive_radiance, 0.013, 0.022); cch2, cch2, cch1)x"},
        {"mhs #1", R"x(cch4=(4, emissive_radiance, 0.059, 0.09); cch2=(2, emissive_radiance, 0.039, 0.068); cch1=(1, emissive_radiance, 0.013, 0.022); cch4, cch2, cch1)x"},
        {"mhs #2", R"x(x=(2, brightness_temperature, 235.000000, 285.000000);lut=lut(lut/generic/rainbow.png);lut(x, 0, 0), lut(x, 0, 1), lut(x, 0, 2))x"},
        {"mhs #3", R"x(x=(4, brightness_temperature, 235.000000, 285.000000);lut=lut(lut/generic/rainbow.png);lut(x, 0, 0), lut(x, 0, 1), lut(x, 0, 2))x"},
        {"mhs #4", R"x(x=(1, brightness_temperature, 200.000000, 300.000000);lut=lut(lut/generic/rainbow.png);lut(x, 0, 0), lut(x, 0, 1), lut(x, 0, 2))x"},
        {"amsu_a #0", R"x(ch5, ch4, ch3)x"},
        {"amsu_a #1", R"x(cch5=(5, brightness_temperature, 220.000000, 280.000000);lut=lut(lut/generic/roygbiv.png);lut(cch5, 0, 0), lut(cch5, 0, 1), lut(cch5, 0, 2))x"},
        {"amsu_a #2", R"x(cch6=(6, brightness_temperature, 210.000000, 260.000000);lut=lut(lut/generic/roygbiv.png);lut(cch6, 0, 0), lut(cch6, 0, 1), lut(cch6, 0, 2))x"},
        {"amsu_a #3", R"x(cch7=(7, brightness_temperature, 210.000000, 240.000000);lut=lut(lut/generic/roygbiv.png);lut(cch7, 0, 0), lut(cch7, 0, 1), lut(cch7, 0, 2))x"},
        {"amsu_a #4", R"x(cch8=(8, brightness_temperature, 200.000000, 250.000000);lut=lut(lut/generic/roygbiv.png);lut(cch8, 0, 0), lut(cch8, 0, 1), lut(cch8, 0, 2))x"},
        {"hirs #0", R"x(ch5, ch4, ch3)x"},
        {"hirs #1", R"x(ch17)x"},
        {"hirs #2", R"x(ch20)x"},
        {"hirs #3", R"x(ch11, ch8, ch6)x"},
        {"hirs #4", R"x(ch19,ch10,ch8)x"},
        {"hirs #5", R"x(ch8,ch10,ch19)x"},
        {"hirs #6", R"x(ch14, ch13, 1)x"},
        {"hirs #7", R"x(cch=(2, brightness_temperature, 213.15, 243.15);lut=lut(lut/cal/bluered.png);lut(cch, 0, 0), lut(cch, 0, 1), lut(cch, 0, 2))x"},
        {"hirs #8", R"x(cch=(3, brightness_temperature, 213.15, 243.15);lut=lut(lut/cal/bluered.png);lut(cch, 0, 0), lut(cch, 0, 1), lut(cch, 0, 2))x"},
        {"hirs #9", R"x(cch=(4, brightness_temperature, 213.15, 243.15);lut=lut(lut/cal/bluered.png);lut(cch, 0, 0), lut(cch, 0, 1), lut(cch, 0, 2))x"},
        {"hirs #10", R"x(cch=(5, brightness_temperature, 223.15, 252.15);lut=lut(lut/cal/bluered.png);lut(cch, 0, 0), lut(cch, 0, 1), lut(cch, 0, 2))x"},
        {"hirs #11", R"x(cch=(6, brightness_temperature, 213.15, 283.15);lut=lut(lut/cal/bluered.png);lut(cch, 0, 0), lut(cch, 0, 1), lut(cch, 0, 2))x"},
        {"hirs #12", R"x(cch=(7, brightness_temperature, 213.15, 293.15);lut=lut(lut/cal/bluered.png);lut(cch, 0, 0), lut(cch, 0, 1), lut(cch, 0, 2))x"},
        {"iasi #0", R"x(ch3800, ch3790, ch3780)x"},
        {"iasi #1", R"x(ch2855, ch2865, ch2875)x"},
        {"iasi #2", R"x((ch3871-ch2904), (ch1546-ch1259), ch3871)x"},
        {"iasi #3", R"x(1-(ch1535+ch1536+ch1537+ch1538+ch1539+ch1540+ch1545+ch1542+ch1543+ch1544+ch1545)/10)x"},
        {"iasi #4", R"x(1-((ch2870 + ch2871 + ch2872 + ch2873 + ch2874 + ch2875 + ch2876 + ch2877 + ch2878 + ch2879 + ch2880 + ch2881 + ch2882 + ch2883 + ch2884 + ch2885 + ch2886) / 16))x"},
        {"iasi #5", R"x((ch140+ch141+ch142)/3)x"},
        {"iasi #6", R"x((ch180+ch181+ch182)/3)x"},
        {"iasi #7", R"x((ch230+ch231+ch232+ch233+ch234)/5)x"},
        {"iasi #8", R"x((ch284+ch283+ch285+ch286+ch287)/5)x"},
        {"iasi #9", R"x((ch351+ch352+ch353+ch354+ch355)/5)x"},
        {"iasi #10", R"x((ch415+ch416+ch417+ch418+ch419)/5)x"},
        {"iasi #11", R"x((ch7096+ch7097+ch7098+ch7099+ch7100+ch7101+ch7102+ch7103+ch7104+ch7105+ch7106)/10)x"},
        {"iasi #12", R"x(1-((ch6259+ch6260+ch6261+ch6262)/4), 1-((ch6171+ch6172+ch6173+ch6174)/4), 1)x"},
        {"iasi #13", R"x((ch284+ch283+ch285+ch286+ch287)/5, (ch230+ch231+ch232+ch233+ch234)/5, (ch180+ch181+ch182)/3)x"},
        {"gome #0", R"x(ch2406, ch2719, ch3367)x"},
        {"gome #1", R"x(ch4096, ch4096, ch3263)x"},
        {"common/automatic #0", R"x(IR11=({10.4um, *, -1, 10}, brightness_temperature_celsius, -60, 40);lm=equp(maps/landmask.png);land=lut(lut/2dfc/2dfc_ir_land.png);sea=lut(lut/2dfc/2dfc_ir_sea.png);lm(0) == 1 ? land(IR11, 0, 0) : sea(IR11, 0, 0),lm(0) == 1 ? land(IR11, 0, 1) : sea(IR11, 0, 1),lm(0) == 1 ? land(IR11, 0, 2) : sea(IR11, 0, 2))x"},
        {"common/automatic #1", R"x(cch4=({10.5um}, brightness_temperature_celsius, -60.000000, 40.000000);lut=lut(lut/2dfc/2dfc_vis.png);lut(cch4,ch{0.86um}^0.9,0),lut(cch4,ch{0.86um}^0.9,1),lut(cch4,ch{0.86um}^0.9,2))x"},
        {"common/automatic #2", R"x(cch2=({0.86um}, sun_angle_compensated_reflective_radiance, 0.000000, 95.050700);cch4=({10.5um}, brightness_temperature_celsius, -60.000000, 40.000000);lut=lut(lut/2dfc/2dfc_vis.png);lut(cch4,cch2^0.9,0),lut(cch4,cch2^0.9,1),lut(cch4,cch2^0.9,2))x"},
        {"common/automatic #3", R"x(VIS=({0.86um}, reflective_radiance, 0.000000, 56.760000);lm=equp(maps/landmask.png);land=lut(lut/2dfc/2dfc_vis_wefax_land.png);sea=lut(lut/2dfc/2dfc_vis_wefax_sea.png);lm(0) == 1 ? land(VIS, 0, 0) : sea(VIS, 0, 0),lm(0) == 1 ? land(VIS, 0, 1) : sea(VIS, 0, 1),lm(0) == 1 ? land(VIS, 0, 2) : sea(VIS, 0, 2))x"},
        {"common/automatic #4", R"x(lut=lut(lut/WXtoImg-HVC-2.png);  lut(ch{0.86um}, ch{10.4um}, 0),  lut(ch{0.86um}, ch{10.4um}, 1),  lut(ch{0.86um}, ch{10.4um}, 2))x"},
        {"common/automatic #5", R"x(cch4=({10.4um, *, -1, 10}, emissive_radiance, 120.000000, 20.000000); bm=equp(maps/map_aweeri.jpg); cch4 * (cch4) + (1-cch4) * bm(0), cch4 * (cch4) + (1-cch4) * bm(1), cch4 * (cch4) + (1-cch4) * bm(2))x"},
        {"common/automatic #6", R"x(cch4=({10.4um, *, -1, 10}, emissive_radiance, 120.000000, 20.000000); bm=equp(maps/nasa_hd.jpg); cch4 * (cch4) + (1-cch4) * bm(0), cch4 * (cch4) + (1-cch4) * bm(1), cch4 * (cch4) + (1-cch4) * bm(2))x"},
        {"common/automatic #7", R"x(cch4=({10.5um} * 4 - 0.01); bm=equp(maps/map_aweeri.jpg); cch4 * (cch4) + (1-cch4) * bm(0), cch4 * (cch4) + (1-cch4) * bm(1), cch4 * (cch4) + (1-cch4) * bm(2))x"},
        {"common/automatic #8", R"x(cch4=({10.5um} * 4 - 0.01); bm=equp(maps/nasa_hd.jpg); cch4 * (cch4) + (1-cch4) * bm(0), cch4 * (cch4) + (1-cch4) * bm(1), cch4 * (cch4) + (1-cch4) * bm(2))x"},
        {"common/automatic #9", R"x(cch1=({0.67um}, sun_angle_compensated_reflective_radiance, 0.000000, 46.203000); bm=equp(maps/map_aweeri.jpg); cch1 * (cch1) + (1-cch1) * bm(0), cch1 * (cch1) + (1-cch1) * bm(1), cch1 * (cch1) + (1-cch1) * bm(2))x"},
        {"common/automatic #10", R"x(cch1=({0.67um}, sun_angle_compensated_reflective_radiance, 0.000000, 46.203000); bm=equp(maps/nasa_hd.jpg); cch1 * (cch1) + (1-cch1) * bm(0), cch1 * (cch1) + (1-cch1) * bm(1), cch1 * (cch1) + (1-cch1) * bm(2))x"},
        {"abi #0", R"x(lut2=lut(lut/goes/abi/wxstar/ch2_curve.png);lut=lut(lut/goes/abi/wxstar/lut.png);ch2 == 1 ? 0 : lut(ch13, lut2(ch2, 0, 0), 0),ch2 == 1 ? 0 : lut(ch13, lut2(ch2, 0, 0), 1), ch2 == 1 ? 0 : lut(ch13, lut2(ch2, 0, 0), 2))x"},
        {"abi #1", R"x(cch1=(1, albedo, 0.000000, 1.000000);cch2=(2, albedo, 0.000000, 1.000000);cch3=(3, albedo, 0.000000, 1.000000);cch2^0.45454545454,(cch2^0.45454545454)*0.45+(cch3^0.45454545454)*0.1+(cch1^0.45454545454)*0.45,cch1^0.45454545454)x"},
        {"abi #2", R"x(bm=equp(maps/nasa_night.jpg);
lut2=lut(lut/goes/abi/wxstar/ch2_curve.png);
lut=lut(lut/goes/abi/usradioguycustomlut.png);
s=(2, sun_angle, 0.000000, 8.000000);
(lut(ch13, lut2(ch2, 0, 0), 0)) * s + (ch13 * (ch13) + (1-ch13) * bm(0)) * (1-s),
(lut(ch13, lut2(ch2, 0, 0), 1)) * s + (ch13 * (ch13) + (1-ch13) * bm(1)) * (1-s),
(lut(ch13, lut2(ch2, 0, 0), 2)) * s + (ch13 * (ch13) + (1-ch13) * bm(2)) * (1-s))x"},
        {"abi #3", R"x(cch13=(13, brightness_temperature, 219.650000, 280.650000);cch5=(5, albedo, 0.010000, 0.590000);cch2=(2, albedo, 0.000000, 0.780000);1-cch13, cch2, cch5)x"},
        {"abi #4", R"x(cch15=(15, brightness_temperature, 163.000000, 300.000000);
cch13=(13, brightness_temperature, 163.000000, 300.000000);
val=(cch13 - cch15 - numerator) / denominator;
numerator=minval/input_range;
denominator=(maxval-minval)/input_range;
minval=-10;
maxval=10;
input_range=137;
lut=lut(lut/cal/abi_split_difference.png);
lut(val, 0, 0),
lut(val, 0, 1),
lut(val, 0, 2))x"},
        {"abi #5", R"x(cch13=(13, brightness_temperature, 163.000000, 330.000000);lut=lut(lut/cal/abi_ir_11-15.png);lut(cch13, 0, 0),lut(cch13, 0, 1),lut(cch13, 0, 2))x"},
        {"abi #6", R"x(cch14=(14, brightness_temperature, 163.000000, 330.000000);lut=lut(lut/cal/abi_ir_11-15.png);lut(cch14, 0, 0),lut(cch14, 0, 1),lut(cch14, 0, 2))x"},
        {"abi #7", R"x(cch15=(15, brightness_temperature, 163.000000, 330.000000);lut=lut(lut/cal/abi_ir_11-15.png);lut(cch15, 0, 0),lut(cch15, 0, 1),lut(cch15, 0, 2))x"},
        {"abi #8", R"x(cch15=(15, brightness_temperature, 163.000000, 330.000000);lut=lut(lut/cal/Dirty_IR_CIRA.png);lut(cch15, 0, 0),lut(cch15, 0, 1),lut(cch15, 0, 2))x"},
        {"abi #9", R"x(cch9=(9, brightness_temperature, 180.000000, 280.000000);lut=lut(lut/cal/abi_wv_8-10.png);lut(cch9, 0, 0),lut(cch9, 0, 1),lut(cch9, 0, 2))x"},
        {"abi #10", R"x(cch8=(8, brightness_temperature, 180.000000, 280.000000);lut=lut(lut/cal/abi_wv_8-10.png);lut(cch8, 0, 0),lut(cch8, 0, 1),lut(cch8, 0, 2))x"},
        {"abi #11", R"x(cch7=(7, brightness_temperature, 190.000000, 400.000000);lut=lut(lut/cal/abi_ir_7.png);lut(cch7, 0, 0),lut(cch7, 0, 1),lut(cch7, 0, 2))x"},
        {"abi #12", R"x(cch13=(13, brightness_temperature, 203.000000, 323.000000);cch2=(2, albedo, 0.000000, 1.000000);cch2^0.58823529411,cch2^0.58823529411,1-cch13)x"},
        {"abi #13", R"x(cch15=(15, brightness_temperature, 163.000000, 330.000000);cch13=(13, brightness_temperature, 163.000000, 330.000000);cch7=(7, brightness_temperature, 163.000000, 330.000000);(cch15-cch13+0.04011976047)/0.05568862275,(cch13-cch7+0.01856287425)/0.0497005988,(cch13-0.48233532934)/0.29401197604)x"},
        {"abi #14", R"x(cch8=(8, brightness_temperature, 233.000000, 253.000000);cch7=(7, brightness_temperature, 273.000000, 338.000000);cch2=(2, albedo, 0.000000, 0.800000);cch7, cch8, cch2)x"},
        {"abi #15", R"x(ch7, ch8, ch9)x"},
        {"abi #16", R"x(ch7,ch13,ch15)x"},
        {"abi #17", R"x(cch13=(13, brightness_temperature, 163.000000, 330.000000);lut=lut(lut/cal/abi_ir_11-15_transparent.png);lut(cch13, 0, 0),lut(cch13, 0, 1),lut(cch13, 0, 2),lut(cch13, 0, 3))x"},
        {"abi #18", R"x(cch14=(14, brightness_temperature, 163.000000, 330.000000);lut=lut(lut/cal/abi_ir_11-15_transparent.png);lut(cch14, 0, 0),lut(cch14, 0, 1),lut(cch14, 0, 2),lut(cch14, 0, 3))x"},
        {"abi #19", R"x(cch15=(15, brightness_temperature, 163.000000, 330.000000);lut=lut(lut/cal/abi_ir_11-15_transparent.png);lut(cch15, 0, 0),lut(cch15, 0, 1),lut(cch15, 0, 2),lut(cch15, 0, 3))x"},
        {"abi #20", R"x(cch7=(7, brightness_temperature, 190.000000, 400.000000);lut=lut(lut/cal/abi_ir_7_transparent.png);lut(cch7, 0, 0),lut(cch7, 0, 1),lut(cch7, 0, 2),lut(cch7, 0, 3))x"},
        {"abi #21", R"x(lut=lut(lut/goes/abi/L2/acha.png);lut(chACHA, 0, 0),lut(chACHA, 0, 1),lut(chACHA, 0, 2))x"},
        {"abi #22", R"x(lut=lut(lut/goes/abi/L2/acht.png);lut(chACHT, 0, 0),lut(chACHT, 0, 1),lut(chACHT, 0, 2))x"},
        {"abi #23", R"x(lut=lut(lut/goes/abi/L2/dsi-cape.png);lut(chDSI, 0, 0),lut(chDSI, 0, 1),lut(chDSI, 0, 2))x"},
        {"abi #24", R"x(lut=lut(lut/goes/abi/L2/tpw.png);lut(chTPW, 0, 0),lut(chTPW, 0, 1),lut(chTPW, 0, 2))x"},
        {"abi #25", R"x(lut=lut(lut/goes/abi/L2/rrqpe.png);lut(chRRQPE, 0, 0),lut(chRRQPE, 0, 1),lut(chRRQPE, 0, 2))x"},
        {"abi #26", R"x(lut=lut(lut/goes/abi/L2/lst.png);lut(chLST, 0, 0),lut(chLST, 0, 1),lut(chLST, 0, 2))x"},
        {"abi #27", R"x(lut=lut(lut/goes/abi/L2/lst_tmbates.png);lut(chLST, 0, 0),lut(chLST, 0, 1),lut(chLST, 0, 2))x"},
        {"abi #28", R"x(lut=lut(lut/goes/abi/L2/sst.png);lut(chSST, 0, 0),lut(chSST, 0, 1),lut(chSST, 0, 2))x"},
        {"abi #29", R"x(lut=lut(lut/goes/abi/L2/sst_tmbates.png);lut(chSST, 0, 0),lut(chSST, 0, 1),lut(chSST, 0, 2))x"},
        {"abi #30", R"x(lut=lut(lut/goes/abi/L2/rrqpe_transparent.png);lut(chRRQPE, 0, 0),lut(chRRQPE, 0, 1),lut(chRRQPE, 0, 2),lut(chRRQPE, 0, 3))x"},
        {"abi #31", R"x(lut=lut(lut/goes/abi/L2/lst_transparent.png);lut(chLST, 0, 0),lut(chLST, 0, 1),lut(chLST, 0, 2),lut(chLST, 0, 3))x"},
        {"abi #32", R"x(lut=lut(lut/goes/abi/L2/lst_transparent_tmbates.png);lut(chLST, 0, 0),lut(chLST, 0, 1),lut(chLST, 0, 2),lut(chLST, 0, 3))x"},
        {"abi #33", R"x(lut=lut(lut/goes/abi/L2/sst_transparent.png);lut(chSST, 0, 0),lut(chSST, 0, 1),lut(chSST, 0, 2),lut(chSST, 0, 3))x"},
        {"abi #34", R"x(lut=lut(lut/goes/abi/L2/sst_transparent_tmbates.png);lut(chSST, 0, 0),lut(chSST, 0, 1),lut(chSST, 0, 2),lut(chSST, 0, 3))x"},
    };

    struct Expected
    {
        const char *name;
        uint64_t hash;
    };

    // Recorded with the bundled muparser (2.3.3 development snapshot) before the switch
    const Expected EXPECTED[] = {
        {"avhrr_3 #0", 0xdc014efe8d8ae8e5ULL},
        {"avhrr_3 #1", 0x02d226680ae2d701ULL},
        {"avhrr_3 #2", 0x55e557461d9d42daULL},
        {"avhrr_3 #3", 0xef837382c6b1e4bcULL},
        {"avhrr_3 #4", 0x3c50b9b2fd868349ULL},
        {"avhrr_3 #5", 0x5ac0955e90f4d7a4ULL},
        {"avhrr_3 #6", 0xd26e901248971644ULL},
        {"avhrr_3 #7", 0x5feb7cd9b2e993b0ULL},
        {"avhrr_3 #8", 0x854e07dc4a1824ceULL},
        {"avhrr_3 #9", 0x4509ff8b7142cb62ULL},
        {"avhrr_3 #10", 0x69f261e0241afc0eULL},
        {"avhrr_3 #11", 0x8ffe0e61aa43bdfbULL},
        {"avhrr_3 #12", 0x2b41a76831c6f386ULL},
        {"avhrr_3 #13", 0xa2912db2da2a0489ULL},
        {"avhrr_3 #14", 0x2c028a7ccd67f241ULL},
        {"avhrr_3 #15", 0x614873ec2d132058ULL},
        {"msu_mr #0", 0xbcb38adc6a8fd863ULL},
        {"msu_mr #1", 0xdc014efe8d8ae8e5ULL},
        {"msu_mr #2", 0x1b24e4fd9dad1db0ULL},
        {"msu_mr #3", 0xa5169dbd2dc4e287ULL},
        {"msu_mr #4", 0xc7b60f1ad792383cULL},
        {"msu_mr #5", 0xa524cdaf6cd1daadULL},
        {"msu_mr #6", 0x4e7e71aebab94cffULL},
        {"msu_mr #7", 0x6b2ffb2ee42cf151ULL},
        {"msu_mr #8", 0xa36c61a99613f644ULL},
        {"msu_mr #9", 0x3ac4e7eb5e6223b0ULL},
        {"msu_mr #10", 0xcb76e650daf898d3ULL},
        {"msu_mr #11", 0xcbb639af8a0e804aULL},
        {"msu_mr #12", 0xc3e5f8a96cf8da52ULL},
        {"msu_mr #13", 0x62271b5dd54d9346ULL},
        {"msu_mr #14", 0x2b41a76831c6f386ULL},
        {"msu_mr #15", 0x2d83b6f10eb1ecbbULL},
        {"msu_mr #16", 0x0137d0356177d5ccULL},
        {"msu_mr #17", 0x0d3ac753de536e0aULL},
        {"mhs #0", 0xfafc4de397d1fe3aULL},
        {"mhs #1", 0xdb8511f2e7367fffULL},
        {"mhs #2", 0x7ed9ce80b6beeb48ULL},
        {"mhs #3", 0x7ed9ce80b6beeb48ULL},
        {"mhs #4", 0x17b9be0d916cece9ULL},
        {"amsu_a #0", 0x1c0de7e30b8fbed8ULL},
        {"amsu_a #1", 0x33ff84848fd4ffb2ULL},
        {"amsu_a #2", 0x7b9e4bf15b7d365cULL},
        {"amsu_a #3", 0xd506f4be7ded25d8ULL},
        {"amsu_a #4", 0x080e902464619dd0ULL},
        {"hirs #0", 0x1c0de7e30b8fbed8ULL},
        {"hirs #1", 0x723f6823eec43534ULL},
        {"hirs #2", 0x92eac4a7f1143338ULL},
        {"hirs #3", 0x0999d7db1e1e3e9cULL},
        {"hirs #4", 0xc6605d065948fd60ULL},
        {"hirs #5", 0x9d8b5b3889a736a4ULL},
        {"hirs #6", 0x94f2e84554588183ULL},
        {"hirs #7", 0xd7c07559eaa4c5f4ULL},
        {"hirs #8", 0xd7c07559eaa4c5f4ULL},
        {"hirs #9", 0xd7c07559eaa4c5f4ULL},
        {"hirs #10", 0x6e8f978db9f13434ULL},
        {"hirs #11", 0x2205b99873e31202ULL},
        {"hirs #12", 0xb4a6f8ae72809b63ULL},
        {"iasi #0", 0x41b2edfad62606baULL},
        {"iasi #1", 0xa4013b97a69524b6ULL},
        {"iasi #2", 0xf18fb3aaeefaba71ULL},
        {"iasi #3", 0xbf36e2e7f8f093a6ULL},
        {"iasi #4", 0xff51fff7dd5be19cULL},
        {"iasi #5", 0x0b50639d7dab2ed1ULL},
        {"iasi #6", 0x6e962784b3adc7c1ULL},
        {"iasi #7", 0x09183230f9a8640dULL},
        {"iasi #8", 0x17550860431afe8bULL},
        {"iasi #9", 0x33a75a0359eb5571ULL},
        {"iasi #10", 0x991275180ba069a9ULL},
        {"iasi #11", 0xec7f56b9f134ca28ULL},
        {"iasi #12", 0x9a14180e8cd21b68ULL},
        {"iasi #13", 0xc0d2a6beb4e5918fULL},
        {"gome #0", 0x9cbb26e29bce2b6eULL},
        {"gome #1", 0x4e2dd280d50d8785ULL},
        {"common/automatic #0", 0x8d77fad42335c73bULL},
        {"common/automatic #1", 0xf03af8efeae83a2bULL},
        {"common/automatic #2", 0x0137d0356177d5ccULL},
        {"common/automatic #3", 0x9af0dd244bd8a4c2ULL},
        {"common/automatic #4", 0x740fd5e5f6fba885ULL},
        {"common/automatic #5", 0x72c4fa6f518091c3ULL},
        {"common/automatic #6", 0x72c4fa6f518091c3ULL},
        {"common/automatic #7", 0xf153fd52eb0eb611ULL},
        {"common/automatic #8", 0xf153fd52eb0eb611ULL},
        {"common/automatic #9", 0x662b0632430e325eULL},
        {"common/automatic #10", 0x662b0632430e325eULL},
        {"abi #0", 0x1a5ebbd64c24bbfaULL},
        {"abi #1", 0x3bb52cb396ada50cULL},
        {"abi #2", 0xe7832c1b95567ac4ULL},
        {"abi #3", 0x3abb8730c315a4c7ULL},
        {"abi #4", 0x6a29af96cb2ce8bcULL},
        {"abi #5", 0x785e703043d118f0ULL},
        {"abi #6", 0xd6b730bddd0bb5feULL},
        {"abi #7", 0x494e5558748cee94ULL},
        {"abi #8", 0x494e5558748cee94ULL},
        {"abi #9", 0x1aa4db1c7ffeba7dULL},
        {"abi #10", 0x1b444714c1f933c3ULL},
        {"abi #11", 0xdb9304fd56f23a22ULL},
        {"abi #12", 0xee51f9b50104dd8dULL},
        {"abi #13", 0x06b5a59de3505707ULL},
        {"abi #14", 0x3af69aa6a68928d9ULL},
        {"abi #15", 0xd1d6a054ed3ba2e8ULL},
        {"abi #16", 0xb663e8fe40c2c98eULL},
        {"abi #17", 0x36f8a9bc7b4c44daULL},
        {"abi #18", 0x095811e971ba8357ULL},
        {"abi #19", 0x3aa51a7653871afcULL},
        {"abi #20", 0x8ac652f2b19fad12ULL},
        {"abi #21", 0x90b0f5aae24992eaULL},
        {"abi #22", 0x32599f9006a00238ULL},
        {"abi #23", 0xc7dcfea263f72af6ULL},
        {"abi #24", 0x3df1f8199352f224ULL},
        {"abi #25", 0x39c86a1f40b052c2ULL},
        {"abi #26", 0x822b925b0a64fb7cULL},
        {"abi #27", 0x822b925b0a64fb7cULL},
        {"abi #28", 0x7781be1b2bcd37f6ULL},
        {"abi #29", 0x7781be1b2bcd37f6ULL},
        {"abi #30", 0x1df6af82ea594c4bULL},
        {"abi #31", 0x80d322a5a8e35da2ULL},
        {"abi #32", 0x80d322a5a8e35da2ULL},
        {"abi #33", 0x6520892608a7ad1bULL},
        {"abi #34", 0x6520892608a7ad1bULL},
    };
} // namespace

TEST_CASE("Composite expressions: every MetOp, NOAA and Meteor composite gives the same values as before")
{
    const size_t n = sizeof(COMPOSITES) / sizeof(COMPOSITES[0]);
    REQUIRE(n == sizeof(EXPECTED) / sizeof(EXPECTED[0]));
    for (size_t i = 0; i < n; i++)
    {
        std::string summary;
        uint64_t h = evaluate_composite(COMPOSITES[i].expression, summary);
        INFO(COMPOSITES[i].name, " (", summary, ")");
        CHECK(std::string(EXPECTED[i].name) == COMPOSITES[i].name);
        CHECK(h == EXPECTED[i].hash);
    }
}

namespace
{
    // SatDump's exceptions end in " => file:line", which says where SatDump threw, not what muparser said
    std::string without_location(const std::string &s) { return s.substr(0, s.find(" => ")); }

    // A parser with two channel variables and a lut() function, as in a composite
    struct TestParser
    {
        double ch1 = 0.25, ch2 = 0.75;
        LutData lut{0.1};
        mu::Parser p;

        TestParser()
        {
            p.DefineVar("ch1", &ch1);
            p.DefineVar("ch2", &ch2);
            p.DefineFunUserData("lut", fake_lut, &lut);
        }
    };

    struct ValueCase
    {
        const char *expression;
        int nout;      // -1: muparser refuses it
        uint64_t bits; // of the first output, or the error code
    };

    // Operators, precedence, built-in functions and constants, and results that are not finite
    const ValueCase VALUE_CASES[] = {
        {"1/0", 1, 0x7ff0000000000000ULL},
        {"-1/0", 1, 0xfff0000000000000ULL},
        {"0/0", 1, 0x7ff8000000000000ULL},
        {"sqrt(-1)", 1, 0x7ff8000000000000ULL},
        {"ln(0)", 1, 0xfff0000000000000ULL},
        {"log(-1)", 1, 0x7ff8000000000000ULL},
        {"(-8)^(1/3)", 1, 0x7ff8000000000000ULL},
        {"-2^2", 1, 0xc010000000000000ULL},
        {"2^3^2", 1, 0x4080000000000000ULL},
        {"1 - - 1", 1, 0x4000000000000000ULL},
        {"-ch1^2", 1, 0xbfb0000000000000ULL},
        {"ch1 == 0.25", 1, 0x3ff0000000000000ULL},
        {"ch1 != ch2", 1, 0x3ff0000000000000ULL},
        {"ch1 <= ch2 && ch2 >= 0.5", 1, 0x3ff0000000000000ULL},
        {"ch1 > 0.5 || ch2 < 0.5", 1, 0x0000000000000000ULL},
        {"_pi", 1, 0x400921fb5444261eULL},
        {"_e", 1, 0x4005bf0a8b145769ULL},
        {"sum(ch1, ch2, 3)", 1, 0x4010000000000000ULL},
        {"avg(ch1, ch2, 3)", 1, 0x3ff5555555555555ULL},
        {"min(ch1, ch2, -3)", 1, 0xc008000000000000ULL},
        {"max(ch1, ch2, 3)", 1, 0x4008000000000000ULL},
        {"rint(2.5)", 1, 0x4008000000000000ULL},
        {"rint(-2.5)", 1, 0xc000000000000000ULL},
        {"sign(-3)", 1, 0xbff0000000000000ULL},
        {"abs(-0)", 1, 0x8000000000000000ULL},
        {"1e308*10", 1, 0x7ff0000000000000ULL},
        {"1e-320", 1, 0x00000000000007e8ULL},
        {"2.5e-3", 1, 0x3f647ae147ae147bULL},
        {"1e3", 1, 0x408f400000000000ULL},
        {".5", 1, 0x3fe0000000000000ULL},
        {"sin(_pi)", 1, 0x3d6be91a62633146ULL},
        {"cos(1)", 1, 0x3fe14a280fb5068cULL},
        {"tan(1)", 1, 0x3ff8eb245cbee3a6ULL},
        {"asin(0.5)", 1, 0x3fe0c152382d7366ULL},
        {"acos(0.5)", 1, 0x3ff0c152382d7366ULL},
        {"atan(1)", 1, 0x3fe921fb54442d18ULL},
        {"sinh(1)", 1, 0x3ff2cd9fc44eb982ULL},
        {"cosh(1)", 1, 0x3ff8b07551d9f550ULL},
        {"tanh(1)", 1, 0x3fe85efab514f394ULL},
        {"asinh(1)", 1, 0x3fec34366179d426ULL},
        {"acosh(2)", 1, 0x3ff5124271980434ULL},
        {"atanh(0.5)", 1, 0x3fe193ea7aad030bULL},
        {"log2(10)", 1, 0x400a934f0979a372ULL},
        {"log10(2)", 1, 0x3fd34413509f79ffULL},
        {"log(10)", 1, 0x40026bb1bbb55516ULL},
        {"exp(1)", 1, 0x4005bf0a8b145769ULL},
        {"ch1^0.58823529411", 1, 0x3fdc50d095c5e9cfULL},
        {"((0.5*max(ch1,min(ch2,ch1)) + 0.5*ch2) / 0.85)^0.65", 1, 0x3fe6aa45d712e671ULL},
        {"ch1*2.2-0.15", 1, 0x3fd999999999999aULL},
        {"ch1*ch2+ch1", 1, 0x3fdc000000000000ULL},
        {"3*ch1 - ch2", 1, 0x0000000000000000ULL},
        {"(3*ch1 + ch2) / 4", 1, 0x3fd8000000000000ULL},
        {"ch1 < 0.065 ? (ch2 - 0.7) * 2.66 : ch2 * 2.2 - 0.15", 1, 0x3ff8000000000001ULL},
        {"ch1 > 0.2 ? ch2 > 0.7 ? 1 : 2 : 3", 1, 0x3ff0000000000000ULL},
        {"lut(ch1, ch2^0.9, 2)", 1, 0x3fad167e33fd0180ULL},
        {"lut(1-ch1, 0, 1) * (ch1) + (1-ch1) * ch2", 1, 0x3fe9ac8b43958106ULL},
        {"ch1, ch2, ch1+ch2", 3, 0x3fd0000000000000ULL},
        {"ch1 = 2", 1, 0x4000000000000000ULL},
        {"ch1 += 1", -1, 0x0000000000000000ULL},
        {"1+2*3-4/5", 1, 0x4018cccccccccccdULL},
        {"(1+2)*(3-4)/5", 1, 0xbfe3333333333333ULL},
        {"2^-1", 1, 0x3fe0000000000000ULL},
        {"-(-ch1)", 1, 0x3fd0000000000000ULL},
        {"ch2^ch1", 1, 0x3fedc783d76af359ULL},
        {"0^0", 1, 0x3ff0000000000000ULL},
        {"0^-1", 1, 0x7ff0000000000000ULL},
        {"(-1)^0.5", 1, 0x7ff8000000000000ULL},
        {"10^308*10", 1, 0x7ff0000000000000ULL},
        {"ch1/0", 1, 0x7ff0000000000000ULL},
        {"-ch1/0", 1, 0xfff0000000000000ULL},
        {"(ch1-0.25)/(ch2-0.75)", 1, 0x7ff8000000000000ULL},
        {"1-ch1/ch2*3+ch2", 1, 0x3fe8000000000000ULL},
        {"ch1*ch1*ch1-ch2", 1, 0xbfe7800000000000ULL},
        {"2*ch1+3*ch2", 1, 0x4006000000000000ULL},
        {"ch1/2+ch2/4", 1, 0x3fd4000000000000ULL},
        {"ch1-0.5*ch2", 1, 0xbfc0000000000000ULL},
    };

    struct ErrorCase
    {
        const char *expression;
        int code; // -1: muparser accepts it
        int pos;
        const char *token;
        const char *message;
    };

    // Expressions muparser refuses: the error code, position, token and message must not change
    const ErrorCase ERROR_CASES[] = {
        {"ch1 +* 2", 0, 5, "*", "Unexpected operator \"*\" found at position 5"},
        {"(ch1", 11, 5, ")", "Missing parenthesis"},
        {"ch1)", 7, 3, ")", "Unexpected parenthesis \")\" at position 3"},
        {"ch1,,ch2", 3, 4, ",", "Unexpected argument separator at position 4"},
        {"ch1 ? 1", 33, -1, "", "If-then-else operator is missing an else clause"},
        {"ch1 ? 1 : ", 2, 11, "", "Unexpected end of expression at position 11"},
        {"ch1 : 1", 34, 5, "", "Misplaced colon at position 5"},
        {"min()", 15, 5, "min", "Too few parameters for function \"min\" at expression position 5"},
        {"lut(ch1, 0)", 15, 10, "lut", "Too few parameters for function \"lut\" at expression position 10"},
        {"lut(ch1, 0, 0, 0)", 14, 16, "lut", "Too many parameters for function \"lut\" at expression position 16"},
        {"foo(ch1)", 1, 0, "foo", "Unexpected token \"foo\" found at position 0."},
        {"ch3 + 1", 1, 0, "ch3", "Unexpected token \"ch3\" found at position 0."},
        {"2^^3", 0, 2, "^", "Unexpected operator \"^\" found at position 2"},
        {"", 25, -1, "", "Expression is empty."},
        {"1.2.3", 5, 3, ".3", "Unexpected value \".3\" found at position 3"},
        {"sin(ch1", 11, 8, ")", "Missing parenthesis"},
        {"\"text\"", 17, 6, "text", "Strings must only be used as function arguments!"},
        {"ch1 2", 5, 4, "2", "Unexpected value \"2\" found at position 4"},
        {"sin", 1, 0, "sin", "Unexpected token \"sin\" found at position 0."},
        {"ch1 + ", 2, 7, "", "Unexpected end of expression at position 7"},
        {"* ch1", 0, 0, "*", "Unexpected operator \"*\" found at position 0"},
        {"ch1 ch2", 6, 4, "ch2", "Unexpected variable \"ch2\" found at position 4"},
        {"1e", 1, 0, "1e", "Unexpected token \"1e\" found at position 0."},
        {"3 = ch1", 0, 2, "=", "Unexpected operator \"=\" found at position 2"},
        {"lut(ch1, 0, 0) lut(ch1, 0, 0)", 12, 15, "lut", "Unexpected function \"lut\" at position 15"},
        {"ch1 ? : 1", 32, 6, ":", "The \":\" operator must be preceded by a closing bracket."},
        {"()", 7, 1, ")", "Unexpected parenthesis \")\" at position 1"},
        {"1,", 2, 3, "", "Unexpected end of expression at position 3"},
        {",1", 3, 0, ",", "Unexpected argument separator at position 0"},
        {"ch1;ch2", 1, 3, ";ch2 ", "Unexpected token \";ch2 \" found at position 3."},
        {"ch1 # 2", 1, 4, "# 2 ", "Unexpected token \"# 2 \" found at position 4."},
        {"ch1 \x01", -1, -1, "", ""},
        {"ch1 % 2", 1, 4, "% 2 ", "Unexpected token \"% 2 \" found at position 4."},
        {"sum()", 15, 5, "sum", "Too few parameters for function \"sum\" at expression position 5"},
        {"lut(ch1 0 0)", 5, 8, "0", "Unexpected value \"0\" found at position 8"},
    };

    satdump::image::Image test_image(int depth, size_t w, size_t h, int channels, int seed)
    {
        satdump::image::Image img(depth, w, h, channels);
        for (size_t i = 0; i < img.size(); i++)
            img.set(i, (int)((i * (7 + seed) + seed * 13 + (i * i) % 17) % (size_t)(img.maxval() + 1)));
        return img;
    }

    uint64_t image_hash(satdump::image::Image &img)
    {
        Hash hash;
        hash.u64(img.depth());
        hash.u64(img.width());
        hash.u64(img.height());
        hash.u64(img.channels());
        if (img.size() > 0)
            hash.bytes(img.raw_data(), img.size() * img.typesize());
        return hash.h;
    }
} // namespace

TEST_CASE("Composite expressions: operators, functions and non-finite results are unchanged")
{
    for (auto &c : VALUE_CASES)
    {
        INFO(std::string(c.expression));
        TestParser t;
        int n = 0;
        uint64_t bits = 0;
        try
        {
            t.p.SetExpr(c.expression);
            double *out = t.p.Eval(n);
            bits = bits_of(out[0]);
        }
        catch (mu::ParserError &e)
        {
            n = -1;
            bits = (uint64_t)e.GetCode();
        }
        CHECK(n == c.nout);
        CHECK(bits == c.bits);
    }
}

TEST_CASE("Composite expressions: invalid expressions give the same error as before")
{
    for (auto &c : ERROR_CASES)
    {
        INFO(std::string(c.expression));
        TestParser t;
        int code = -1, pos = -1;
        std::string token, message;
        try
        {
            t.p.SetExpr(c.expression);
            int n = 0;
            t.p.Eval(n);
        }
        catch (mu::ParserError &e)
        {
            code = e.GetCode();
            pos = e.GetPos();
            token = e.GetToken();
            message = e.GetMsg();
        }
        CHECK(code == c.code);
        CHECK(pos == c.pos);
        CHECK(token == c.token);
        CHECK(message == c.message);
    }
}


// The one difference between the bundled 2.3.3 snapshot and Debian's 2.3.4 that a user could
// meet: the longest expression muparser takes grew from 4999 to 19999 characters (in every
// released muparser since 2.3.3). Composites are far shorter (those in resources/ are under
// 2000 characters, macros expanded). Linux builds use the system's copy and must take 19999;
// Windows, macOS and Android keep the bundled copy's 4999 (utils/muparser.h makes the choice).
TEST_CASE("Composite expressions: expressions up to 19999 characters are accepted")
{
#ifdef SATDUMP_SYSTEM_MUPARSER
    const int longest = 19999; // Debian's muparser 2.3.4
#else
    const int longest = 4999; // the bundled 2.3.3 snapshot
#endif
    CHECK(mu::MaxLenExpression == longest + 1);

    struct LengthCase
    {
        int length;
        int code; // -1: accepted
        double value;
    };
    const LengthCase cases[] = {
        {4999, -1, 2500.0}, // accepted by both
        // Refused by the bundled copy (code 37, expression too long), accepted by Debian's
        {5001, longest >= 5001 ? -1 : mu::ecEXPRESSION_TOO_LONG, longest >= 5001 ? 2501.0 : 0.0},
        {longest, -1, (longest + 1) / 2.0},
        {longest + 2, mu::ecEXPRESSION_TOO_LONG, 0.0},
    };
    for (auto &c : cases)
    {
        std::string e = "1";
        while ((int)e.size() < c.length)
            e += "+1";
        REQUIRE((int)e.size() == c.length);
        INFO(c.length, " characters");
        TestParser t;
        int code = -1;
        double v = 0;
        try
        {
            t.p.SetExpr(e);
            v = t.p.Eval();
        }
        catch (mu::ParserError &err)
        {
            code = err.GetCode();
        }
        CHECK(code == c.code);
        CHECK(v == c.value);
    }
}

TEST_CASE("Composite expressions: image::generate_image_expression gives the same pictures")
{
    satdump::image::Image a = test_image(8, 9, 7, 1, 1);
    satdump::image::Image rgb = test_image(16, 9, 7, 3, 2);
    std::vector<satdump::image::ExpressionChannel> channels(2);
    channels[0].tkt = "a";
    channels[0].img = &a;
    channels[1].tkt = "r,g,b";
    channels[1].img = &rgb;

    struct
    {
        const char *expression;
        uint64_t hash;
    } cases[] = {
        {"a*0.5+r, g-b, a>0.5?r:b", 0x2d6d03df7e3a28c1ULL},
        {"r^0.65, (g/0.85)^0.65, min(a,b)*2", 0x660008a8fcbab482ULL},
        {"1-a", 0xdf2f41825ffcca0cULL},
    };
    for (auto &c : cases)
    {
        INFO(std::string(c.expression));
        satdump::image::Image out = satdump::image::generate_image_expression(channels, c.expression);
        CHECK(image_hash(out) == c.hash);
    }

    std::string msg;
    try
    {
        satdump::image::generate_image_expression(channels, "a +* r");
    }
    catch (const std::exception &e)
    {
        msg = without_location(e.what());
    }
    CHECK(msg == "Unexpected operator \"*\" found at position 3");
}

TEST_CASE("Composite expressions: products::generate_expression_product_composite gives the same pictures")
{
    satdump::products::ImageProduct product;
    const char *names[] = {"1", "2", "3a", "4", "5"};
    for (int i = 0; i < 5; i++)
    {
        satdump::products::ImageProduct::ImageHolder h;
        h.abs_index = i;
        h.channel_name = names[i];
        h.image = test_image(16, 11, 9, 1, 3 + i);
        product.images.push_back(h);
    }

    struct
    {
        const char *expression;
        uint64_t hash;
    } cases[] = {
        {"ch2, ch2, ch1", 0xffff0361e268d49dULL},
        {"ch1 < 0.065 ? (ch4 - 0.7) * 2.66 : ch2 * 2.2 - 0.15, ch1 < 0.065 ? (ch4 - 0.7) * 2.66 : ch2 * 2.2 - 0.15, ch1 < 0.065 ? (ch4 - 0.7) * 2.66 : ch1 * 2.2 - 0.15",
         0xb0a68237867816a2ULL},
        {"xx=(ch1) * 1.5;xx * (xx) + (1-xx) * ch2, ch2, ch3a", 0x5c36320c7857784bULL},
        {"1 - ch5", 0x3e3055a5558c74e6ULL},
        {"ref_channel=2;ch1, ch2, (ch4 - ch5) / 2 + 0.5", 0x9ea49b9034ddf54aULL},
    };
    for (auto &c : cases)
    {
        INFO(std::string(c.expression));
        satdump::image::Image out = satdump::products::generate_expression_product_composite(&product, c.expression);
        CHECK(image_hash(out) == c.hash);
    }

    std::string msg;
    try
    {
        satdump::products::generate_expression_product_composite(&product, "ch1 +* 2");
    }
    catch (const std::exception &e)
    {
        msg = without_location(e.what());
    }
    CHECK(msg == "Unexpected operator \"*\" found at position 4");
}
