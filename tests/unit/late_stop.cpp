// Fork: inject SIGTERM during flag parsing, after command entry and before any source opens.
#include "common/cli_utils.h"
#include "core/cli/stop_signals.h"
#include "init.h"
#include "logger.h"
#include "satdump_vars.h"
#include <csignal>
#include <cstdlib>
static nlohmann::json stopped_flags(int argc, char **argv, std::map<std::string, std::optional<std::type_index>> types = {})
{
    auto flags = parse_common_flags(argc, argv, types);
    std::raise(SIGTERM);
    logger->info("Injected stop after command entry");
    return flags;
}
#define parse_common_flags stopped_flags
#ifdef SATDUMP_TEST_LIVE_STOP
#include "../../src-cli/legacy/live.cpp"
#else
#include "../../src-cli/legacy/record.cpp"
#endif
#undef parse_common_flags
int main(int argc, char **argv)
{
    satdump::cli::catch_stop_signals();
    initLogger();
    const char *prefix = std::getenv("SATDUMP_TEST_PREFIX");
    if (!prefix) return 5;
    satdump::RESPATH = std::string(prefix) + "/share/satdump/";
    satdump::LIBPATH = std::string(prefix) + "/" SATDUMP_TEST_INSTALL_LIBDIR "/satdump/";
    satdump::initSatDump();
#ifdef SATDUMP_TEST_LIVE_STOP
    int result = main_live(argc, argv);
#else
    int result = main_record(argc, argv);
#endif
    satdump::exitSatDump();
    return result;
}
