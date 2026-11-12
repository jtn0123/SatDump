#pragma once

#include "libs/ctpl/ctpl_stl.h"
#include "pipeline.h"
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace satdump
{
    namespace pipeline
    {
        class LivePipeline
        {
        private:
            Pipeline d_pipeline;
            nlohmann::json d_parameters;
            std::string d_output_dir;

        private:
            std::vector<std::future<void>> module_futs;

            // Fork: errors of the modules that failed in their thread, by position in the module list.
            // Shared with the module tasks, so a task never needs the LivePipeline itself
            struct Failures
            {
                std::mutex mtx;
                std::map<size_t, std::string> errors;
            };
            std::shared_ptr<Failures> failures;

            void prepare_modules(std::vector<int> mods);
            void prepare_module(std::string id);
            void start_modules(ctpl::thread_pool &tp);
            static void module_failed(ProcessingModule &mod, Failures &fails, size_t index, std::string error);

        public:
            LivePipeline(Pipeline pipeline, nlohmann::json parameters, std::string output_dir);
            ~LivePipeline();

            std::vector<std::shared_ptr<ProcessingModule>> modules;

            void start(std::shared_ptr<dsp::stream<complex_t>> stream, ctpl::thread_pool &tp, bool server = false); // Start everything locally, "as usual", or in "server" mode
            void start_client(ctpl::thread_pool &tp);                                                               // Start a "client" setup, with no local input stream
            void stop();                                                                                            // Stop everything. The LivePipeline is NOT re-usable

            std::string getOutputFile();

            nlohmann::json getModulesStats();

            // Fork: true once any module has failed in its thread; getFailures() says which and why
            bool has_failed();
            std::string getFailures();

            void drawUIs();
        };
    } // namespace pipeline
} // namespace satdump
