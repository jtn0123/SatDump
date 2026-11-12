// Members that were read before anything had set them: the stats and UI values of three
// decoders, and the header fields of a Meteor LRPT picture segment. Each object is built in
// memory first filled with the byte 0xA5, so a member its constructor leaves unset keeps
// that pattern instead of happening to be 0.
#include "doctest.h"
#include "meteor/instruments/msumr/lrpt/segment.h"
#include "meteor/module_meteor_lrpt_decoder.h"
#include "metop/module_metop_ahrpt_decoder.h"
#include "metop/module_metop_dump_decoder.h"
#include "nlohmann/json.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

using meteor::msumr::lrpt::Segment;

namespace
{
    void fill_a5(void *p, size_t n) { std::memset(p, 0xA5, n); }

    // Called through a volatile pointer, so the compiler cannot drop the fill as a dead store
    // to memory that a constructor is about to take over (GCC's -flifetime-dse does that)
    void (*volatile fill_pattern)(void *, size_t) = fill_a5;

    template <typename T>
    struct DestroyAndFree
    {
        void operator()(T *p) const
        {
            p->~T();
            ::operator delete(p);
        }
    };

    template <typename T>
    using PatternPtr = std::unique_ptr<T, DestroyAndFree<T>>;

    // Constructs a T in memory filled with 0xA5
    template <typename T, typename... Args>
    PatternPtr<T> make_over_pattern(Args &&...args)
    {
        static_assert(alignof(T) <= __STDCPP_DEFAULT_NEW_ALIGNMENT__, "T needs more alignment than operator new gives");
        void *mem = ::operator new(sizeof(T));
        fill_pattern(mem, sizeof(T));
        try
        {
            return PatternPtr<T>(new (mem) T(std::forward<Args>(args)...));
        }
        catch (...)
        {
            ::operator delete(mem);
            throw;
        }
    }

    template <typename V>
    int count_nonzero(const V *v, int n)
    {
        int c = 0;
        for (int i = 0; i < n; i++)
            if (v[i] != 0)
                c++;
        return c;
    }

    // Read access to the decoders' protected members
    class AhrptProbe final : public metop::MetOpAHRPTDecoderModule
    {
    public:
        using MetOpAHRPTDecoderModule::MetOpAHRPTDecoderModule;
        const int *rs_errors() const { return errors; }
        const float *ber_plot() const { return ber_history; }
    };

    class DumpProbe final : public metop::MetOpDumpDecoderModule
    {
    public:
        using MetOpDumpDecoderModule::MetOpDumpDecoderModule;
        const int *rs_errors() const { return errors; }
        int correlation() const { return cor; }
        const float *cor_plot() const { return cor_history; }
    };

    class LrptProbe final : public meteor::METEORLRPTDecoderModule
    {
    public:
        using METEORLRPTDecoderModule::METEORLRPTDecoderModule;
        const int *rs_errors() const { return errors; }
        int correlation() const { return cor; }
        const float *ber_plot() const { return ber_history; }
        const float *cor_plot() const { return cor_history; }
    };

    // The settings of the Meteor-M pipelines: M2 (correlator), or M2-x (interleaved, Viterbi)
    nlohmann::json lrpt_params(bool m2x)
    {
        nlohmann::json p;
        p["diff_decode"] = m2x;
        if (m2x)
        {
            p["m2x_mode"] = true;
            p["interleaved"] = true;
            p["viterbi_ber_thresold"] = 0.3;
            p["viterbi_outsync_after"] = 20;
        }
        return p;
    }
} // namespace

// The stats are read with a streaming input (live mode), where they hold no file progress

TEST_CASE("MetOp AHRPT decoder: before the first frame, stats show no Reed-Solomon errors (were left unset)")
{
    auto m = make_over_pattern<AhrptProbe>("in.soft", "out", metop::MetOpAHRPTDecoderModule::getParams());
    m->setInputType(satdump::pipeline::DATA_STREAM);
    nlohmann::json stats = m->getModuleStats();
    CHECK(stats["rs_avg"].get<int>() == 0);
    CHECK(stats["deframer_state"].get<std::string>() == "NOSYNC");
    CHECK(count_nonzero(m->rs_errors(), 4) == 0);
    CHECK(count_nonzero(m->ber_plot(), 200) == 0);
}

TEST_CASE("Meteor LRPT decoder, M2-x: before the first buffer, stats say NOSYNC with no errors (were left unset)")
{
    auto m = make_over_pattern<LrptProbe>("in.soft", "out", lrpt_params(true));
    m->setInputType(satdump::pipeline::DATA_STREAM);
    nlohmann::json stats = m->getModuleStats();
    CHECK(stats["viterbi_lock"].get<int>() == 0);
    CHECK(stats["viterbi_state"].get<std::string>() == "NOSYNC");
    CHECK(stats["viterbi_ber"].get<float>() == 0.0f);
    CHECK(stats["rs_avg"].get<int>() == 0);
    CHECK(stats["deframer_state"].get<std::string>() == "NOSYNC");
    CHECK(count_nonzero(m->rs_errors(), 4) == 0);
    CHECK(count_nonzero(m->ber_plot(), 200) == 0);
}

TEST_CASE("Meteor LRPT decoder, M2: before the first frame, stats and UI values are 0 (were left unset)")
{
    auto m = make_over_pattern<LrptProbe>("in.soft", "out", lrpt_params(false));
    m->setInputType(satdump::pipeline::DATA_STREAM);
    nlohmann::json stats = m->getModuleStats();
    CHECK(stats["rs_avg"].get<int>() == 0);
    CHECK(stats["lock_state"].get<std::string>() == "NOSYNC");
    CHECK(m->correlation() == 0);
    CHECK(count_nonzero(m->rs_errors(), 4) == 0);
    CHECK(count_nonzero(m->ber_plot(), 200) == 0);
    CHECK(count_nonzero(m->cor_plot(), 200) == 0);
}

TEST_CASE("MetOp X-band dump decoder: before the first frame, UI values are 0 (were left unset)")
{
    auto m = make_over_pattern<DumpProbe>("in.soft", "out", nlohmann::json::object());
    m->setInputType(satdump::pipeline::DATA_STREAM);
    CHECK(m->getModuleStats()["lock_state"].get<std::string>() == "NOSYNC");
    CHECK(m->correlation() == 0);
    CHECK(count_nonzero(m->rs_errors(), 4) == 0);
    CHECK(count_nonzero(m->cor_plot(), 200) == 0);
}

TEST_CASE("Meteor LRPT segment: a missing segment has known header values and is not valid (were left unset)")
{
    // What std::map::operator[] makes for every segment that was not received
    auto s = make_over_pattern<Segment>();
    CHECK_FALSE(s->valid);
    CHECK(s->partial);
    CHECK(s->day_time == 0);
    CHECK(s->ms_time == 0u);
    CHECK(s->us_time == 0);
    CHECK(s->timestamp == -1.0);
    CHECK(s->MCUN == 0);
    CHECK(s->QT == 0);
    CHECK(s->DC == 0);
    CHECK(s->AC == 0);
    CHECK(s->QFM == 0);
    CHECK(s->QF == 0);
    CHECK_FALSE(s->isValid());
}

TEST_CASE("Meteor LRPT segment: a packet with no picture data gives an invalid segment with known values (were left unset)")
{
    std::vector<uint8_t> pkt(14, 0); // the 14-byte header and nothing after it
    auto s = make_over_pattern<Segment>(pkt.data(), (int)pkt.size(), false, false);
    CHECK_FALSE(s->valid);
    CHECK_FALSE(s->partial);
    CHECK(s->ms_time == 0u);
    CHECK(s->timestamp == -1.0);
    CHECK(s->QT == 0);
    CHECK(s->DC == 0);
    CHECK(s->AC == 0);
    CHECK(s->QFM == 0);
    CHECK(s->QF == 0);
    CHECK_FALSE(s->isValid());
}

TEST_CASE("Meteor LRPT segment: a good packet still decodes and is valid")
{
    // Header: MCU number 28, QT 0, DC/AC tables 0, QFM 0xFFF0, quality factor 80. Then 14
    // blocks that each hold only "DC difference 0" (00) and "end of block" (1010): flat grey
    std::vector<uint8_t> pkt(14 + 16, 0);
    pkt[8] = 28;
    pkt[11] = 0xFF;
    pkt[12] = 0xF0;
    pkt[13] = 80;
    size_t bit = 0;
    for (int b = 0; b < 14; b++)
        for (bool v : {false, false, true, false, true, false})
        {
            if (v)
                pkt[14 + bit / 8] |= 0x80 >> (bit % 8);
            bit++;
        }

    Segment s(pkt.data(), (int)pkt.size(), false, true);
    CHECK(s.valid);
    CHECK_FALSE(s.partial);
    CHECK(s.MCUN == 28);
    CHECK(s.QF == 80);
    CHECK(s.isValid());
    int not_grey = 0;
    for (const auto &row : s.lines)
        for (uint8_t px : row)
            if (px != 128)
                not_grey++;
    CHECK(not_grey == 0);

    s.QFM = 0x1234; // a header the reader does not accept
    CHECK_FALSE(s.isValid());
}
