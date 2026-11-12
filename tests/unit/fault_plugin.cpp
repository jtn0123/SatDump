// Fork test support: a plugin with the modules of fault_module.h, which fail on purpose. Built for
// tests/cli/thread_faults.sh only, and never installed.

#include "core/plugin.h"
#include "fault_module.h"
#include "pipeline/module.h"

class ForkFaultTestPlugin : public satdump::Plugin
{
public:
    std::string getID() override { return "fork_fault_test"; }

    void init() override { satdump::eventBus->register_handler<satdump::pipeline::RegisterModulesEvent>(registerModules); }

    static void registerModules(const satdump::pipeline::RegisterModulesEvent &evt) { fork_test::register_modules(evt.modules_registry); }
};

PLUGIN_LOADER(ForkFaultTestPlugin)
