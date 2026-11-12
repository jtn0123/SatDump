#include "core/plugin.h"
#include "pipeline.h"

#include "core/config.h"
#include "core/exception.h"
#include "logger.h"
#include "pipeline/module.h"
#include <atomic>
#include <exception>
#include <mutex>
#include <thread>

namespace satdump
{
    namespace pipeline
    {
        namespace
        {
            // Fork: puts modules on the UI's list and always takes them off again, also when a module
            // throws (a failed module used to stay on the list)
            class UiCallListScope
            {
            private:
                bool d_ui;
                std::shared_ptr<std::vector<std::shared_ptr<ProcessingModule>>> d_list;
                std::shared_ptr<std::mutex> d_mtx;

            public:
                UiCallListScope(bool ui, std::shared_ptr<std::vector<std::shared_ptr<ProcessingModule>>> list, std::shared_ptr<std::mutex> mtx,
                                std::vector<std::shared_ptr<ProcessingModule>> mods)
                    : d_ui(ui), d_list(list), d_mtx(mtx)
                {
                    if (!d_ui)
                        return;
                    std::lock_guard<std::mutex> lock(*d_mtx);
                    for (auto &m : mods)
                        d_list->push_back(m);
                }

                ~UiCallListScope()
                {
                    if (!d_ui)
                        return;
                    std::lock_guard<std::mutex> lock(*d_mtx);
                    d_list->clear();
                }

                UiCallListScope(const UiCallListScope &) = delete;
                UiCallListScope &operator=(const UiCallListScope &) = delete;
            };

            // Fork: the first error thrown in a module thread, kept so it can be thrown again on the
            // calling thread once both threads are joined
            class ThreadErrors
            {
            private:
                std::mutex d_mtx;
                std::exception_ptr d_first;

            public:
                void keep(std::exception_ptr e)
                {
                    std::lock_guard<std::mutex> lock(d_mtx);
                    if (!d_first)
                        d_first = e;
                }

                bool any()
                {
                    std::lock_guard<std::mutex> lock(d_mtx);
                    return (bool)d_first;
                }

                void rethrow_first()
                {
                    std::lock_guard<std::mutex> lock(d_mtx);
                    if (d_first)
                        std::rethrow_exception(d_first);
                }
            };

            // Fork: runs a module in its own thread. An exception leaving the thread used to end the
            // whole program (std::terminate); now it is logged and kept. Returns false when it failed
            bool run_module(ProcessingModule &mod, ThreadErrors &errors)
            {
                try
                {
                    mod.process();
                    return true;
                }
                catch (const std::exception &e)
                {
                    logger->error("Module %s failed: %s", mod.getIDM().c_str(), e.what());
                    errors.keep(std::current_exception());
                }
                catch (...)
                {
                    logger->error("Module %s failed with an unknown error", mod.getIDM().c_str());
                    errors.keep(std::make_exception_ptr(satdump_exception("Module " + mod.getIDM() + " failed with an unknown error")));
                }
                return false;
            }

            // Fork: stops a module whose process() failed. Demodulators start their own DSP blocks in
            // process() and only stop them at its end, so a failed one used to be destroyed with its
            // blocks still running, which still ended the program ("pure virtual method called").
            // An error while stopping is logged; the first error is the one reported
            void stop_after_failure(ProcessingModule &mod)
            {
                try
                {
                    mod.stop();
                }
                catch (const std::exception &e)
                {
                    logger->error("Module %s failed to stop after its error: %s", mod.getIDM().c_str(), e.what());
                }
                catch (...)
                {
                    logger->error("Module %s failed to stop after its error, with an unknown error", mod.getIDM().c_str());
                }
            }

            // Fork: runs a module on the calling thread; when it fails it is stopped before the error
            // goes on to the caller (see stop_after_failure)
            void process_or_stop(ProcessingModule &mod)
            {
                try
                {
                    mod.process();
                }
                catch (...)
                {
                    stop_after_failure(mod);
                    throw;
                }
            }
        } // namespace

        void Pipeline::run(std::string input_file, std::string output_directory, nlohmann::json parameters, std::string input_level, bool ui,
                           std::shared_ptr<std::vector<std::shared_ptr<ProcessingModule>>> uiCallList, std::shared_ptr<std::mutex> uiCallListMutex, std::string *final_file)
        {
            if (!std::filesystem::exists(input_file))
            {
                logger->error("Input file " + input_file + " does not exist!");
                return;
            }

            logger->debug("Starting " + id);

            std::string lastFile = "";

            int currentStep = 0;
            bool foundLevel = false;

            /*
            In most cases, all processing pipelines will start from
            baseband, demodulate to some intermediate level and then
            feed an actual decoder down to frames.

            Hence, in most cases, if the first and second module both
            support streaming data from the first to the second, we
            can skip this intermediate level and do both in parrallel.

            Here, we first test modules are compatible with this way
            of doing things, then unless specifically disabled by the
            user, proceed to run both in parallel saving up on processing
            time.
            */
            if (input_level == "baseband" && parameters.count("disable_multi_modules") == 0 && steps.size() > 2 && steps[1].module != "" && steps[2].module != "")
            {
                logger->info("Checking the 2 first modules...");

                PipelineStep module1 = steps[1];
                PipelineStep module2 = steps[2];

                if (!moduleExists(module1.module) || !moduleExists(module2.module))
                    throw satdump_exception("Module " + module1.module + " or " + module2.module + " is not registered. Cancelling pipeline.");

                nlohmann::json params1 = prepareParameters(module1.parameters, parameters);
                nlohmann::json params2 = prepareParameters(module2.parameters, parameters);

                std::shared_ptr<ProcessingModule> m1 =
                    getModuleInstance(module1.module, module1.input_override == "" ? input_file : output_directory + "/" + module1.input_override, output_directory + "/" + id, params1);
                std::shared_ptr<ProcessingModule> m2 =
                    getModuleInstance(module2.module, module2.input_override == "" ? input_file : output_directory + "/" + module2.input_override, output_directory + "/" + id, params2);

                std::vector<ModuleDataType> m1_outputs = m1->getOutputTypes();
                std::vector<ModuleDataType> m2_inputs = m2->getInputTypes();

                bool m1_has_stream = std::find(m1_outputs.begin(), m1_outputs.end(), DATA_STREAM) != m1_outputs.end();
                bool m2_has_stream = std::find(m2_inputs.begin(), m2_inputs.end(), DATA_STREAM) != m2_inputs.end();

                if (m1_has_stream && m2_has_stream)
                {
                    logger->info("Both 2 first modules can be run at once!");

                    m1->setInputType(DATA_FILE);
                    m1->setOutputType(DATA_STREAM);
                    m1->output_fifo = std::make_shared<dsp::RingBuffer<uint8_t>>(1000000);

                    m2->input_fifo = m1->output_fifo;
                    m2->setInputType(DATA_STREAM);
                    m2->setOutputType(DATA_FILE);
                    m2->input_active = true;

                    m1->init();
                    m2->init();

                    UiCallListScope ui_list(ui, uiCallList, uiCallListMutex, {m1, m2});

                    // Fork: an exception in either module thread used to end the program. Now it is kept,
                    // the other module is stopped and joined, and the error is thrown again here
                    ThreadErrors errors;
                    std::atomic<bool> m2_finished(false);

                    std::thread module1_thread(
                        [&m1, &errors]()
                        {
                            // Fork: nothing else stops module 1, so a failed one is stopped here
                            if (!run_module(*m1, errors))
                                stop_after_failure(*m1);
                        });
                    std::thread module2_thread(
                        [&m2, &errors, &m2_finished]()
                        {
                            // Fork: nobody reads the buffer any more, so writes into it return at once
                            // instead of leaving module 1 waiting on a full buffer forever
                            if (!run_module(*m2, errors))
                                m2->input_fifo->stopWriter();
                            m2_finished = true;
                        });

                    if (module1_thread.joinable())
                        module1_thread.join();
                    // Fork: stop waiting for module 2 to empty the buffer once it has ended or a module
                    // failed (this loop used to wait forever when module 2 was gone)
                    while (!m2_finished && !errors.any() && m2->input_fifo->getReadable() > 0)
                        std::this_thread::sleep_for(std::chrono::seconds(1));
                    m2->input_active = false;
                    m2->input_fifo->stopReader();
                    m2->input_fifo->stopWriter();
                    try // Fork: a throwing stop() used to skip the join below, which ended the program
                    {
                        m2->stop();
                    }
                    catch (const std::exception &e)
                    {
                        logger->error("Module %s failed to stop: %s", m2->getIDM().c_str(), e.what());
                        errors.keep(std::current_exception());
                    }
                    catch (...)
                    {
                        logger->error("Module %s failed to stop with an unknown error", m2->getIDM().c_str());
                        errors.keep(std::make_exception_ptr(satdump_exception("Module " + m2->getIDM() + " failed to stop")));
                    }
                    if (module2_thread.joinable())
                        module2_thread.join();

                    errors.rethrow_first();

                    lastFile = m2->getOutput();
                    currentStep += 2;
                    input_level = steps[2].level;
                }
            }

            for (; currentStep < (int)steps.size(); currentStep++)
            {
                PipelineStep &step = steps[currentStep];

                if (!foundLevel)
                {
                    foundLevel = step.level == input_level;
                    logger->info("Data is already at level " + step.level + ", skipping");
                    continue;
                }

                logger->info("Processing data to level " + step.level);

                std::vector<std::string> files;

                {
                    // Check module exists!
                    if (!moduleExists(step.module))
                        throw satdump_exception("Module " + step.module + " is not registered. Cancelling pipeline.");

                    nlohmann::json final_parameters = prepareParameters(step.parameters, parameters);

                    std::shared_ptr<ProcessingModule> module =
                        getModuleInstance(step.module, step.input_override == "" ? (lastFile == "" ? input_file : lastFile) : output_directory + "/" + step.input_override, output_directory + "/" + id,
                                          final_parameters);

                    module->setInputType(DATA_FILE);
                    module->setOutputType(DATA_FILE);

                    module->init();

                    {
                        UiCallListScope ui_list(ui, uiCallList, uiCallListMutex, {module});
                        process_or_stop(*module);
                    }

                    lastFile = module->getOutput();

                    module.reset();
                }
            }

            // We are done. Does this have a dataset?
            bool input_is_dataset = std::filesystem::path(input_file).stem().string() == "dataset" && std::filesystem::path(input_file).extension().string() == ".json";
            if ((std::filesystem::exists(output_directory + "/dataset.json") || input_is_dataset) && satdump_cfg.shouldAutoprocessProducts())
            {
                logger->debug("Products processing is enabled! Starting processing module.");

                std::string dataset_path = output_directory + "/dataset.json";
                if (input_is_dataset)
                    dataset_path = input_file;

                // It does, fire up the processing module.
                std::shared_ptr<ProcessingModule> module = getModuleInstance("products_processor", dataset_path, output_directory + "/" + id, "");

                module->setInputType(DATA_FILE);
                module->setOutputType(DATA_FILE);

                module->init();

                {
                    UiCallListScope ui_list(ui, uiCallList, uiCallListMutex, {module});
                    process_or_stop(*module);
                }

                module.reset();
            }

            eventBus->fire_event<events::PipelineDoneProcessingEvent>({id, output_directory});

            if (final_file != nullptr)
                *final_file = lastFile;
        }

        nlohmann::json Pipeline::prepareParameters(nlohmann::json &module_params, nlohmann::json &pipeline_params)
        {
            nlohmann::json final_parameters = module_params;
            for (const nlohmann::detail::iteration_proxy_value<nlohmann::detail::iter_impl<nlohmann::json>> &param : pipeline_params.items())
                if (final_parameters.count(param.key()) > 0)
                    final_parameters[param.key()] = param.value();
                else
                    final_parameters.emplace(param.key(), param.value());

            logger->debug("Parameters :");
            for (const nlohmann::detail::iteration_proxy_value<nlohmann::detail::iter_impl<nlohmann::json>> &param : final_parameters.items())
                logger->debug("   - " + param.key() + " : " + param.value().dump());

            return final_parameters;
        }
    } // namespace pipeline
} // namespace satdump