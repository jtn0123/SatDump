#include "core/resources.h"
#include <filesystem>
#include "satdump_vars.h"
#include "logger.h"
#include "utils/file.h"

namespace resources
{
    // Fork: the checks in the current folder (portable mode) use existsInCurrentFolder(), which
    // never throws: a current folder SatDump may not read stopped every command at start-up
    bool resourceExists(std::string resource)
    {
        if (satdump::existsInCurrentFolder("resources"))
            return satdump::existsInCurrentFolder("resources/" + resource);
        else
            return std::filesystem::exists(satdump::RESPATH + "resources/" + resource);
    }

    std::string getResourcePath(std::string resource)
    {
        if (satdump::existsInCurrentFolder("resources"))
        {
            if (!satdump::existsInCurrentFolder("resources/" + resource))
                logger->error("Resources " + resource + " does not exist!");
            return "resources/" + resource;
        }
        else
        {
            if (!std::filesystem::exists(satdump::RESPATH + "resources/" + resource))
                logger->error("Resources " + resource + " does not exist!");
            return satdump::RESPATH + "resources/" + resource;
        }
    }
}