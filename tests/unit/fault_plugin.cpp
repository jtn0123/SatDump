// Fork test support: a plugin with the modules of fault_module.h, which fail on purpose. Built for
// tests/cli/thread_faults.sh only, and never installed.

#include "core/plugin.h"
#include "core/cli/cli.h"
#include "fault_module.h"
#include "pipeline/module.h"

namespace
{
    // Fork: exercise the actual process boundary with a non-standard command exception.
    class UnknownFaultCommand : public satdump::CmdHandler
    {
    public:
        UnknownFaultCommand() : CmdHandler("fork_unknown") {}
        void reg(CLI::App *app) override { app->add_subcommand(cmd); }
        void run(CLI::App *, CLI::App *, bool) override { throw 7; }
    };
}
class ForkFaultTestPlugin : public satdump::Plugin
{
public:
    std::string getID() override { return "fork_fault_test"; }

    void init() override
    {
        satdump::eventBus->register_handler<satdump::pipeline::RegisterModulesEvent>(registerModules);
        satdump::eventBus->register_handler<satdump::cli::RegisterSubcommandEvent>([](auto event)
        { event.cmd_handlers.push_back(std::make_shared<UnknownFaultCommand>()); });
    }

    static void registerModules(const satdump::pipeline::RegisterModulesEvent &evt) { fork_test::register_modules(evt.modules_registry); }
};

PLUGIN_LOADER(ForkFaultTestPlugin)
