#include "legacy.h"
#include "init.h"
#include "logger.h"

int main_old(int argc, char *argv[]);

namespace satdump
{
    void LegacyCmdHandler::reg(CLI::App *app)
    {
        CLI::App *sub = app->add_subcommand("legacy", "Run legacy command from pre-2.0.0-style satdump CLI");
        sub->prefix_command();
        sub->set_help_flag();
    }

    void LegacyCmdHandler::run(CLI::App *app, CLI::App *subcom, bool is_gui)
    {
        auto args = subcom->remaining();

        std::vector<char *> vec;
        args.insert(args.begin(), "satdump legacy");
        for (auto &a : args)
            vec.push_back((char *)a.c_str());

        const int code = main_old(vec.size(), vec.data());

        // Fork: a legacy command that finished (code 0) returns, like every other command, so
        // CommandHandler::run() reports a DSP block that failed during it and the program ends
        // with exit code 2: the output is incomplete. It always called exit() here, so such a run
        // ended with 0. Its own codes (1 usage or settings error, 3 radio problem) still end the
        // program at once, as before
        if (code == 0)
            return;

        // exit() skips main()'s cleanup, so shut SatDump down first
        satdump::exitSatDump();
        exit(code);
    }
} // namespace satdump