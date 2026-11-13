#pragma once

#include "common/dsp/complex.h"
#include "common/dsp/resamp/polyphase_bank.h"
#include "dsp/block.h"
#include "pipeline/module.h"
#include <memory>

namespace satdump
{
    namespace ndsp
    {
        class ModuleWrapperBlock : public Block
        {
        public:
            std::string module_id;
            nlohmann::json mod_cfg;
            std::shared_ptr<pipeline::ProcessingModule> mod;

            std::thread modThread;
            std::atomic<bool> work2shouldrun{false}; // Fork: shared with the output worker.
            bool module_needs_stop = false;
            std::thread work2Thread;

            int output_read_size = 1024;

        private:
            bool work();
            void work2();

        public:
            ModuleWrapperBlock();
            ~ModuleWrapperBlock();

        protected:
            // Fork: the base serialises these with its own worker's start and join.
            void start_extra() override;
            void stop_extra() noexcept override;

        public:

            nlohmann::ordered_json get_cfg_list()
            {
                nlohmann::ordered_json p;
                add_param_simple(p, "module", "string");
                if (is_work_running())
                    p["module"]["disabled"] = true;
                add_param_simple(p, "cfg", "json");
                if (is_work_running())
                    p["cfg"]["disabled"] = true;
                add_param_simple(p, "outsize", "json");
                if (is_work_running())
                    p["outsize"]["disabled"] = true;
                add_param_simple(p, "status", "stat");
                return p;
            }

            nlohmann::json get_cfg(std::string key)
            {
                if (key == "module")
                    return module_id;
                else if (key == "cfg")
                    return mod_cfg;
                else if (key == "outsize")
                    return output_read_size;
                else if (key == "stat")
                    return mod->getModuleStats();
                else
                    throw satdump_exception(key);
            }

            cfg_res_t set_cfg(std::string key, nlohmann::json v)
            {
                if (key == "module")
                    module_id = v;
                else if (key == "cfg")
                    mod_cfg = v;
                else if (key == "outsize")
                    output_read_size = v;
                else if (key == "stat")
                    ;
                else
                    throw satdump_exception(key);
                return RES_OK;
            }
        };
    } // namespace ndsp
} // namespace satdump