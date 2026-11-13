#include "core/cli/cli.h"
#include "core/plugin.h"
#include "init.h"
#include "legacy.h"
#include "logger.h"
#include "core/cli/stop_signals.h"
#include <memory>

int main(int argc, char *argv[])
{
    // Fork: catch a station stop before plugins, resources or databases can take time.
    satdump::cli::catch_stop_signals();

    // Init logger
    initLogger();

    // Basic flags
    bool verbose = satdump::cli::checkVerbose(argc, argv);

    // Init SatDump, silent or verbose as requested
    if (!verbose)
        logger->set_level(slog::LOG_WARN);
    satdump::initSatDump();
    completeLoggerInit();
    if (!verbose)
        logger->set_level(slog::LOG_TRACE);

    if (satdump::cli::stop_requested())
    {
        logger->warn("Signal Received. Stopping.");
        satdump::exitSatDump();
        return 0;
    }

    satdump::eventBus->register_handler<satdump::cli::RegisterSubcommandEvent>([](const satdump::cli::RegisterSubcommandEvent &evt)
                                                                               { evt.cmd_handlers.push_back(std::make_shared<satdump::LegacyCmdHandler>()); });

    satdump::cli::CommandHandler h;
    // Exit codes: 0 = success, 1 = usage error, 2 = the command failed
    if (h.parse(argc, argv) != 0)
    {
        // CLI11 has already printed the reason; its own codes (105-115) are folded into 1
        satdump::exitSatDump();
        return 1;
    }
    if (satdump::cli::stop_requested())
    {
        logger->warn("Signal Received. Stopping.");
        satdump::exitSatDump();
        return 0;
    }
    const bool failed = h.run();

    satdump::exitSatDump();
    return failed ? 2 : 0;
}
