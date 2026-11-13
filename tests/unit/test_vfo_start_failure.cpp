// Fork: compile the actual VFO implementation with a shifter that fails inside start().
#include "common/dsp/path/splitter_vfo.h"
#include "doctest.h"
#include <system_error>
namespace dsp
{
    class FailingShifter : public FreqShiftBlock
    {
    public:
        using FreqShiftBlock::FreqShiftBlock;
        static inline bool after_start = false;
        ~FailingShifter()
        {
            // Fork: report a missed rollback before a joinable worker can outlive this object.
            if (d_thread.joinable()) std::_Exit(13);
        }
        static inline int stopped = 0;
        void start() override
        {
            if (after_start) FreqShiftBlock::start();
            throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
        }
        void stop() override { FreqShiftBlock::stop(); stopped++; }
    };
}
#define FreqShiftBlock FailingShifter
#include "../../src-core/common/dsp/path/splitter_vfo.cpp"
#undef FreqShiftBlock
TEST_CASE("VFO start failure releases settings lock and removes partial VFO")
{
    for (bool started : {false, true})
    {
        dsp::FailingShifter::after_start = started;
        dsp::FailingShifter::stopped = 0;
        dsp::VFOSplitterBlock splitter(std::make_shared<dsp::stream<complex_t>>());
        splitter.start();
        CHECK_THROWS_AS(splitter.add_vfo("failed", 6000000, 0), std::system_error);
        CHECK_FALSE(splitter.get_vfo_output("failed"));
        splitter.del_vfo("failed");
        splitter.stop();
        CHECK(dsp::FailingShifter::stopped == 1);
    }
}
