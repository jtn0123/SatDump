// Fork: deliberately exports the ABI used before the DSP virtual hooks were added.
// Plugin itself has the same interface in ABI 1; no DSP lifecycle method is called here.
#include "core/plugin.h"
#include "logger.h"

class OldAbiPlugin : public satdump::Plugin
{
public:
    std::string getID() override { return "fork_old_abi_test"; }
    void init() override { logger->error("Old ABI plugin init was called"); }
};

extern "C"
{
    PLUGIN_DLL const extern int SATDUMP_ABI_VERSION = 1;
    satdump::Plugin *loader()
    {
        logger->error("Old ABI plugin loader was called");
        return new OldAbiPlugin();
    }
}
