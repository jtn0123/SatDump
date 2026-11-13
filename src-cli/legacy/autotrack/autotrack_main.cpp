#include "autotrack.h"

#include "init.h"
#include "logger.h"
#include <filesystem>
#include "core/cli/stop_signals.h"

int main_autotrack(int argc, char *argv[])
{
    // Fork: a request remembered during CLI startup must not engage a radio.
    if (satdump::cli::stop_requested())
    {
        logger->warn("Signal Received. Stopping.");
        return 0;
    }

    if (argc < 3) // Check overall command
    {
        logger->error("Usage : " + std::string(argv[0]) + " autotrack autotrack_config.json");
        logger->error("Sample command :");
        logger->error("./satdump autotrack autotrack_config.json");
        return 1;
    }

    if (!std::filesystem::exists(argv[2]))
    {
        logger->critical("%s does not exist! Exiting", argv[2]);
        return 1;
    }

    logger->error("CLI AutoTrack is still WIP!");

    nlohmann::json settings = loadJsonFile(argv[2]);
    nlohmann::json parameters = settings["parameters"];
    std::string output_folder = settings["output_folder"];

    // Create output dir
    if (!std::filesystem::exists(output_folder))
        std::filesystem::create_directories(output_folder);

    AutoTrackApp *app = new AutoTrackApp(settings, parameters, output_folder);

    logger->info("Setup Done!");

    // Now, we wait
    while (1)
    {
        if (satdump::cli::stop_requested())
        {
            logger->warn("Signal Received. Stopping.");
            break;
        }

        app->web_heartbeat();

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    delete app;

    return 0;
}
