// Fork: compile the real record command with test-only start overrides. The FFT throws the
// same system_error as a failed std::thread creation; the splitter records whether it started.
// This avoids production fault switches and dynamic-loader interposition in sanitizer builds.
#include "common/dsp/fft/fft_pan.h"
#include "common/dsp/path/splitter.h"
#include "core/cli/stop_signals.h"
#include "init.h"
#include "logger.h"
#include "satdump_vars.h"
#include <cstdlib>
#include <system_error>

namespace
{
    bool splitter_started = false;
}
namespace dsp
{
    class FailingFFTPanBlock : public FFTPanBlock
    {
    public:
        using FFTPanBlock::FFTPanBlock;
        void start() override
        {
            // Fork: Block::start() sets this before thread creation can throw.
            should_run = true;
            logger->warn("Injected FFT thread-start failure");
            throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
        }
    };
    class ObservedSplitterBlock : public SplitterBlock
    {
    public:
        using SplitterBlock::SplitterBlock;
        void start() override
        {
            splitter_started = true;
            SplitterBlock::start();
        }
    };
}
#define FFTPanBlock FailingFFTPanBlock
#define SplitterBlock ObservedSplitterBlock
#include "../../src-cli/legacy/record.cpp"
#undef SplitterBlock
#undef FFTPanBlock

int main(int argc, char **argv)
{
    satdump::cli::catch_stop_signals();
    initLogger();
    // Fork: the harness stays in the build folder; load resources/plugins from the CLI install.
    const char *prefix = std::getenv("SATDUMP_TEST_PREFIX");
    if (prefix == nullptr)
        return 5;
    satdump::RESPATH = std::string(prefix) + "/share/satdump/";
    satdump::LIBPATH = std::string(prefix) + "/" SATDUMP_TEST_INSTALL_LIBDIR "/satdump/";
    satdump::initSatDump();
    int result = 0;
    try
    {
        result = main_record(argc, argv);
    }
    catch (const std::system_error &e)
    {
        logger->error("Expected FFT start error: %s", e.what());
        result = splitter_started ? 4 : 2;
    }
    catch (const std::exception &e)
    {
        logger->error("Unexpected record start error: %s", e.what());
        result = 5;
    }
    catch (...)
    {
        logger->error("Unexpected record start error: unknown exception");
        result = 5;
    }
    satdump::exitSatDump();
    return result;
}
