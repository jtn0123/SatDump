#include "live_pipeline.h"
#include "core/exception.h"
#include "logger.h"
#include "pipeline/module.h"

namespace satdump
{
    namespace pipeline
    {
        LivePipeline::LivePipeline(Pipeline pipeline, nlohmann::json parameters, std::string output_dir)
            : d_pipeline(pipeline), d_parameters(parameters), d_output_dir(output_dir), failures(std::make_shared<Failures>())
        {
        }

        // Fork: modules still running are stopped here (they used to keep running on the thread pool
        // after the LivePipeline was gone)
        LivePipeline::~LivePipeline() { stop(); }

        void LivePipeline::prepare_modules(std::vector<int> mods)
        {
            for (int currentModule : mods)
            {
                if ((int)d_pipeline.steps.size() <= currentModule)
                    throw satdump_exception("Invalid live pipeline step!");

                // Prep parameters
                nlohmann::json final_parameters = Pipeline::prepareParameters(d_pipeline.steps[currentModule].parameters, d_parameters);

                // Log it all out
                logger->info("Module " + d_pipeline.steps[currentModule].module);
                logger->debug("Parameters :");
                for (const nlohmann::detail::iteration_proxy_value<nlohmann::detail::iter_impl<nlohmann::json>> &param : final_parameters.items())
                    logger->debug("   - " + param.key() + " : " + param.value().dump());

                modules.push_back(getModuleInstance(d_pipeline.steps[currentModule].module, "", d_output_dir + "/" + d_pipeline.id, final_parameters));
            }
        }

        void LivePipeline::prepare_module(std::string id)
        {
            // Log it all out
            logger->info("Module " + id);
            logger->debug("Parameters :");
            for (const nlohmann::detail::iteration_proxy_value<nlohmann::detail::iter_impl<nlohmann::json>> &param : d_parameters.items())
                logger->debug("   - " + param.key() + " : " + param.value().dump());

            modules.push_back(getModuleInstance(id, "", d_output_dir + "/" + d_pipeline.id, d_parameters));
        }

        void LivePipeline::module_failed(ProcessingModule &mod, Failures &fails, size_t index, std::string error)
        {
            logger->error("Live module %s failed: %s", mod.getIDM().c_str(), error.c_str());
            {
                std::lock_guard<std::mutex> lock(fails.mtx);
                fails.errors[index] = error;
            }

            // Nothing reads this module's input any more: let whatever writes into it carry on
            // instead of waiting on a full buffer (which stalled the whole chain, recording included)
            if (mod.getInputType() == DATA_DSP_STREAM && mod.input_stream)
                mod.input_stream->stopWriter();
            else if (mod.getInputType() == DATA_STREAM && mod.input_fifo)
                mod.input_fifo->stopWriter();
        }

        // Fork: modules are only started once all of them are set up, so a module that cannot be set
        // up no longer leaves the ones before it running with nobody to stop them. A module that throws
        // in its thread is logged and its input unblocked; the other modules keep going. The tasks
        // hold the module and the error list themselves (they used to use the LivePipeline's own list)
        void LivePipeline::start_modules(ctpl::thread_pool &tp)
        {
            try
            {
                for (size_t i = 0; i < modules.size(); i++)
                {
                    std::shared_ptr<ProcessingModule> mod = modules[i];
                    std::shared_ptr<Failures> fails = failures;
                    mod->input_active = true;
                    module_futs.push_back(tp.push(
                        [mod, fails, i](int)
                        {
                            logger->info("Start processing...");
                            try
                            {
                                mod->process();
                            }
                            catch (const std::exception &e)
                            {
                                module_failed(*mod, *fails, i, e.what());
                            }
                            catch (...)
                            {
                                module_failed(*mod, *fails, i, "unknown error");
                            }
                        }));
                }
            }
            catch (...)
            {
                stop();
                throw;
            }
        }

        void LivePipeline::start(std::shared_ptr<dsp::stream<complex_t>> stream, ctpl::thread_pool &tp, bool server)
        {
            if (server) // Server mode
            {
                if (d_pipeline.live_cfg.server_live.size() == 0)
                    throw satdump_exception("Pipeline does not support server mode!");

                prepare_modules(d_pipeline.live_cfg.server_live);
                d_parameters["pkt_size"] = d_pipeline.live_cfg.pkt_size;
                prepare_module("network_server");
            }
            else // Normal mode
            {
                prepare_modules(d_pipeline.live_cfg.normal_live);
            }

            // Fork: a pipeline without live modules used to read past the end of the module list
            if (modules.size() == 0)
                throw satdump_exception("Pipeline has no live modules!");

            // Init first module in the chain, always a demod...
            modules[0]->input_stream = stream;
            modules[0]->setInputType(DATA_DSP_STREAM);
            modules[0]->setOutputType(modules.size() > 1 ? DATA_STREAM : DATA_FILE);
            modules[0]->output_fifo = std::make_shared<dsp::RingBuffer<uint8_t>>(1000000);
            modules[0]->init();

            // Init whatever's in the middle
            for (int i = 1; i < (int)modules.size() - 1; i++)
            {
                modules[i]->input_fifo = modules[i - 1]->output_fifo;
                modules[i]->output_fifo = std::make_shared<dsp::RingBuffer<uint8_t>>(1000000);
                modules[i]->setInputType(DATA_STREAM);
                modules[i]->setOutputType(DATA_STREAM);
                modules[i]->init();
            }

            // Init the last module
            if (modules.size() > 1)
            {
                int num = modules.size() - 1;
                modules[num]->input_fifo = modules[num - 1]->output_fifo;
                modules[num]->setInputType(DATA_STREAM);
                modules[num]->setOutputType(DATA_FILE);
                modules[num]->init();
            }

            start_modules(tp);
        }

        void LivePipeline::start_client(ctpl::thread_pool &tp)
        {
            // Init modules
            {
                if (d_pipeline.live_cfg.client_live.size() == 0)
                    throw satdump_exception("Pipeline does not support client mode!");

                d_parameters["pkt_size"] = d_pipeline.live_cfg.pkt_size;
                prepare_module("network_client");
                prepare_modules(d_pipeline.live_cfg.client_live);
            }

            // Init the first and whatever's in the middle
            for (int i = 0; i < (int)modules.size() - 1; i++)
            {
                if (i > 0)
                    modules[i]->input_fifo = modules[i - 1]->output_fifo;
                modules[i]->output_fifo = std::make_shared<dsp::RingBuffer<uint8_t>>(1000000);
                modules[i]->setInputType(DATA_STREAM);
                modules[i]->setOutputType(DATA_STREAM);
                modules[i]->init();
            }

            // Init the last module
            if (modules.size() > 1)
            {
                int num = modules.size() - 1;
                modules[num]->input_fifo = modules[num - 1]->output_fifo;
                modules[num]->setInputType(DATA_STREAM);
                modules[num]->setOutputType(DATA_FILE);
                modules[num]->init();
            }

            start_modules(tp);
        }

        // Fork: stop() never throws (a module that had failed made it throw half-way, leaving the
        // modules after it running), and it does nothing when no module was started, or when called
        // a second time (it used to read past the end of the task list after a failed start)
        void LivePipeline::stop()
        {
            if (module_futs.size() == 0)
                return;

            logger->info("Stop processing");
            for (size_t i = 0; i < modules.size(); i++)
            {
                std::shared_ptr<ProcessingModule> mod = modules[i];

                mod->input_active = false;

                if (mod->getInputType() == DATA_DSP_STREAM && mod->input_stream)
                {
                    mod->input_stream->stopReader();
                    mod->input_stream->stopWriter();
                }
                else if (mod->getInputType() == DATA_STREAM && mod->input_fifo)
                {
                    mod->input_fifo->stopReader();
                    mod->input_fifo->stopWriter();
                }

                if (i >= module_futs.size()) // Never started
                    continue;

                try
                {
                    mod->stop();
                }
                catch (const std::exception &e)
                {
                    logger->error("Live module %s failed to stop: %s", mod->getIDM().c_str(), e.what());
                }
                catch (...)
                {
                    logger->error("Live module %s failed to stop with an unknown error", mod->getIDM().c_str());
                }

                try
                {
                    if (module_futs[i].valid())
                        module_futs[i].get();
                }
                catch (const std::exception &e)
                {
                    logger->error("Live module %s task failed: %s", mod->getIDM().c_str(), e.what());
                }
                catch (...)
                {
                    logger->error("Live module %s task failed with an unknown error", mod->getIDM().c_str());
                }
            }
            module_futs.clear();
        }

        std::string LivePipeline::getOutputFile()
        {
            if (modules.size() == 0) // Fork: there is no output without modules (this read past the end of the list)
                return "";
            int num = modules.size() - 1;
            return modules[num]->getOutput();
        }

        nlohmann::json LivePipeline::getModulesStats()
        {
            std::map<size_t, std::string> errors;
            {
                std::lock_guard<std::mutex> lock(failures->mtx);
                errors = failures->errors;
            }

            nlohmann::json v;
            for (size_t i = 0; i < modules.size(); i++)
            {
                std::shared_ptr<ProcessingModule> mod = modules[i];
                v[mod->getIDM()] = mod->getModuleStats();
                // Fork: a module that failed says so in its stats
                if (errors.count(i) && (v[mod->getIDM()].is_null() || v[mod->getIDM()].is_object()))
                    v[mod->getIDM()]["error"] = errors[i];
            }
            return v;
        }

        bool LivePipeline::has_failed()
        {
            std::lock_guard<std::mutex> lock(failures->mtx);
            return failures->errors.size() > 0;
        }

        std::string LivePipeline::getFailures()
        {
            std::lock_guard<std::mutex> lock(failures->mtx);
            std::string out;
            for (auto &e : failures->errors)
            {
                if (out.size() > 0)
                    out += "; ";
                out += (e.first < modules.size() ? modules[e.first]->getIDM() : std::to_string(e.first)) + ": " + e.second;
            }
            return out;
        }

        void LivePipeline::drawUIs()
        {
            for (std::shared_ptr<ProcessingModule> mod : modules)
                mod->drawUI(true);
        }
    } // namespace pipeline
} // namespace satdump
