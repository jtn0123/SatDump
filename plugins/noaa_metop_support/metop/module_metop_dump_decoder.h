#pragma once

#include "pipeline/modules/base/filestream_to_filestream.h"

namespace metop
{
    class MetOpDumpDecoderModule : public satdump::pipeline::base::FileStreamToFileStreamModule
    {
    protected:
        uint8_t *buffer;

        bool locked = false;
        // Fork: start at 0. The UI reads these before the first frame is decoded, and without
        // a starting value they held whatever was in memory
        int errors[4] = {0, 0, 0, 0};
        int cor = 0;

        // UI Stuff
        float cor_history[200] = {};

    public:
        MetOpDumpDecoderModule(std::string input_file, std::string output_file_hint, nlohmann::json parameters);
        ~MetOpDumpDecoderModule();
        void process();
        void drawUI(bool window);
        nlohmann::json getModuleStats();

    public:
        static std::string getID();
        virtual std::string getIDM() { return getID(); };
        static nlohmann::json getParams() { return {}; } // TODOREWORK
        static std::shared_ptr<satdump::pipeline::ProcessingModule> getInstance(std::string input_file, std::string output_file_hint, nlohmann::json parameters);
    };
} // namespace metop