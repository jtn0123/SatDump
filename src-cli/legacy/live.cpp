#include "live.h"
#include "exit_codes.h"
#include "common/cli_utils.h"
#include "common/dsp/fft/fft_pan.h"
#include "common/dsp/path/splitter.h"
#include "common/dsp/path/splitter_vfo.h"
#include "common/dsp_source_sink/dsp_sample_source.h"
#include "core/exception.h"
#include "init.h"
#include "logger.h"
#include "pipeline/live_pipeline.h"
#include "webserver.h"
#ifdef SATDUMP_TEST_LIVE_RUNNING
#include "core/plugin.h"
#include "live_running_event.h"
#endif
#include <cinttypes>
#include <algorithm>
#include "core/cli/stop_signals.h"
#include <cmath>
#include <filesystem>

// Fork: starts the web server asked for with --http_server. False when it cannot start, most
// often because another program uses the port, or the value is not an address and port (a
// number, or a flag given no value); the reason is logged. The live run then stops with exit
// code 1, so whatever watches it through /api learns at once that it cannot. The run used to go
// on with nothing answering there, and a value that was not text threw, which ended the run
// with exit code 2.
static bool start_webserver(const nlohmann::json &parameters) { return webserver::start_from_flag(parameters.at("http_server")); }

// TODOREWORK the timeout and stats are partially broken here now!

// Fork: a live module that failed during the run is reported once everything has stopped, and the
// command ends with exit code 2 (it used to end with 0, or not end at all)
static void throw_if_failed(std::map<std::string, satdump::pipeline::LivePipeline *> pipelines)
{
    std::string failures;
    for (auto &p : pipelines)
    {
        if (!p.second->has_failed())
            continue;
        if (failures.size() > 0)
            failures += "; ";
        failures += (p.first.size() > 0 ? p.first + ": " : "") + p.second->getFailures();
    }
    if (failures.size() > 0)
        throw satdump_exception("Live pipeline failed: " + failures);
}

int main_live(int argc, char *argv[])
{
    // Fork: a preceding generic command may have restored default handlers. Reinstall without
    // clearing a stop remembered during startup or between commands.
    satdump::cli::catch_stop_signals();

    // Fork: preserve a stop received during CLI startup, before opening any radio.
    if (satdump::cli::stop_requested())
    {
        logger->warn("Signal Received. Stopping.");
        return 0;
    }

    if (argc < 5) // Check overall command
    {
        logger->error("Usage : " + std::string(argv[0]) + " live [pipeline_id] [output_file_or_directory] [additional options as required]");
        logger->error("Extra options (examples. Any parameter used in modules or sources can be used here) :");
        logger->error(" --samplerate [baseband_samplerate] --baseband_format [f32/s16/s8/u8] --dc_block --iq_swap");
        logger->error(" --source [airspy/rtlsdr/etc] --gain 20 --bias");
        logger->error("As well as --timeout in seconds");
        logger->error("Sample command :");
        logger->error("./satdump live metop_ahrpt metop_output_directory --source airspy --samplerate 6e6 --frequency 1701.3e6 --general_gain 18 --bias --timeout 780");
        return 1;
    }

    std::string downlink_pipeline = argv[2];
    std::string output_file = argv[3];

    // Parse flags
    nlohmann::json parameters = parse_common_flags(argc - 4, &argv[4], {{"source_id", typeid(std::string)}});

    // Fork: getPipelineFromID() throws for an unknown ID rather than returning an empty
    // optional. Reject that usage error before radio setup, instead of reporting exit 2.
    const auto &known_pipelines = satdump::pipeline::pipelines;
    if ((parameters.contains("client") || !parameters.contains("multi_vfo")) &&
        std::none_of(known_pipelines.begin(), known_pipelines.end(), [&](const auto &pipeline) { return pipeline.id == downlink_pipeline; }))
    {
        logger->error("Pipeline %s does not exist!", downlink_pipeline.c_str());
        return 1;
    }

    // Fork: validate every VFO before opening a radio, and retain this exact configuration
    // for startup so a second file read cannot bypass validation.
    nlohmann::json validated_multi_cfg;
    if (!parameters.contains("client") && parameters.contains("multi_vfo"))
    {
        try
        {
            const auto path = parameters["multi_vfo"].get<std::string>();
            // Fork: the loader returns null for a missing file; distinguish it from an
            // explicit null/empty list, which the old items() loop accepted.
            if (!std::filesystem::exists(path))
                throw std::invalid_argument("the VFO configuration file does not exist");
            validated_multi_cfg = loadJsonFile(path);
            if (!validated_multi_cfg.is_object() && !validated_multi_cfg.is_array() && !validated_multi_cfg.is_null())
                throw std::invalid_argument("the VFO configuration must be an object, array or null");
            for (const auto &entry : validated_multi_cfg.items())
            {
                auto &vfo = entry.value();
                // Fork: startup historically promoted missing/null parameters to an object
                // while injecting DSP settings. Normalize before retaining the validated config.
                if (vfo.is_object() && (!vfo.contains("parameters") || vfo["parameters"].is_null()))
                    vfo["parameters"] = nlohmann::json::object();
                if (!vfo.is_object() || !vfo.contains("frequency") || !vfo["frequency"].is_number() ||
                    !vfo.contains("pipeline") || !vfo["pipeline"].is_string() ||
                    !vfo.contains("parameters") || !vfo["parameters"].is_object())
                    throw std::invalid_argument("VFO " + entry.key() + " requires a numeric frequency, a pipeline ID and an object of parameters");
                const std::string id = vfo["pipeline"].get<std::string>();
                if (std::none_of(known_pipelines.begin(), known_pipelines.end(), [&](const auto &pipeline) { return pipeline.id == id; }))
                    throw std::invalid_argument("VFO " + entry.key() + " names unknown pipeline " + id);
            }
        }
        catch (const std::exception &e)
        {
            logger->error("Invalid multi-VFO configuration: %s", e.what());
            return 1;
        }
    }

    // Init SatDump
    satdump::tle_file_override = parameters.contains("tle_override") ? parameters["tle_override"].get<std::string>() : "";

    if (parameters.contains("client"))
    {
        logger->info("Starting in client mode!");

        // Create output dir
        if (!std::filesystem::exists(output_file))
            std::filesystem::create_directories(output_file);

        // Get pipeline
        std::optional<satdump::pipeline::Pipeline> pipeline = satdump::pipeline::getPipelineFromID(downlink_pipeline);

        if (!pipeline.has_value())
        {
            logger->critical("Pipeline " + downlink_pipeline + " does not exist!");
            return 1;
        }

        // Fork: the thread pool is made before the pipeline, so it goes after it. It went first, and
        // waited for ever on the modules still running in it when an error left this block early
        ctpl::thread_pool live_thread_pool(8);

        // Init pipeline
        std::unique_ptr<satdump::pipeline::LivePipeline> live_pipeline = std::make_unique<satdump::pipeline::LivePipeline>(pipeline.value(), parameters, output_file);

        // Attempt to start the source and pipeline
        try
        {
            // Fork: client mode has no radio; cancel before opening its network source too.
            if (satdump::cli::stop_requested())
                return 0;
            live_pipeline->start_client(live_thread_pool);
        }
        catch (std::exception &e)
        {
            logger->error("Fatal error running pipeline/device : " + std::string(e.what()));
            return 1;
        }

        // Fork: an error once the pipeline runs stops it (and the webserver) before it is reported
        bool webserver_started = false;
        try
        {
            // If requested, boot up webserver
            if (parameters.contains("http_server"))
            {
                webserver::handle_callback = [&live_pipeline]() { return live_pipeline->getModulesStats().dump(4); };
                if (!start_webserver(parameters))
                {
                    live_pipeline->stop();
                    return 1;
                }
                webserver_started = true;
            }

            // Now, we wait
            while (1)
            {
                if (satdump::cli::stop_requested())
                {
                    logger->warn("Signal Received. Stopping.");
                    break;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            // Stop cleanly
            live_pipeline->stop();
        }
        catch (...)
        {
            live_pipeline->stop();
            if (webserver_started)
                webserver::stop();
            throw;
        }

        if (webserver_started)
            webserver::stop();

        throw_if_failed({{"", live_pipeline.get()}});
    }
    else
    {
        uint64_t samplerate;
        uint64_t frequency;
        uint64_t timeout;
        std::string handler_id;
        std::string hdl_dev_id;

        try
        {
            samplerate = parameters["samplerate"].get<uint64_t>();
            frequency = parameters["frequency"].get<uint64_t>();
            timeout = parameters.contains("timeout") ? parameters["timeout"].get<uint64_t>() : 0;
            handler_id = parameters["source"].get<std::string>();
            if (parameters.contains("source_id"))
                hdl_dev_id = parameters["source_id"].get<std::string>();
        }
        catch (std::exception &e)
        {
            logger->error("Error parsing arguments! %s", e.what());
            return 1;
        }

        // Create output dir
        if (!std::filesystem::exists(output_file))
            std::filesystem::create_directories(output_file);

        // Get the sources of the type asked for. Fork: only that driver is asked for its radios
        // (it was every driver, and some search the network for radios: UHD, PlutoSDR, SoapySDR)
        dsp::registerAllSources();
        std::vector<dsp::SourceDescriptor> source_tr = dsp::getAvailableSourcesOfType(handler_id);
        dsp::SourceDescriptor selected_src;

        for (dsp::SourceDescriptor src : source_tr)
            logger->debug("Device " + src.name);

        // Try to find it and check it's usable
        bool src_found = false;
        for (dsp::SourceDescriptor src : source_tr)
        {
            if (handler_id == src.source_type)
            {
                if (parameters.contains("source_id"))
                {
                    if (hdl_dev_id == src.unique_id)
                    {
                        selected_src = src;
                        src_found = true;
                    }
                }
                else
                {
                    selected_src = src;
                    src_found = true;
                }
            }
        }

        if (!src_found)
        {
            logger->error("Could not find a handler for source type : %s!", handler_id.c_str());
            return satdump::EXIT_RADIO; // Fork: no such radio connected (or its driver is missing)
        }

        // Init source
        std::shared_ptr<dsp::DSPSampleSource> source_ptr = getSourceFromDescriptor(selected_src);
        try // Fork: a radio that cannot be opened or set up is exit code 3
        {
            // Fork: setup and source discovery may have received a stop after command entry.
            if (satdump::cli::stop_requested())
                return 0;
            source_ptr->open();
            source_ptr->set_frequency(frequency);
            source_ptr->set_samplerate(samplerate);
            source_ptr->set_settings(parameters);
        }
        catch (std::exception &e)
        {
            logger->error("Radio problem: could not open or set up the radio: %s", e.what());
            return satdump::EXIT_RADIO;
        }

        if (parameters.contains("multi_vfo"))
        {
            logger->info("Starting in multi VFO mode!");
            logger->critical("This is still considered WIP!");

            const nlohmann::json &multi_cfg = validated_multi_cfg;

            // Fork: construct the pool before the radio runs, so a thread-creation failure
            // cannot strand it outside the cleanup handler. The pool outlives the pipelines.
            ctpl::thread_pool live_thread_pool(128);
            std::unique_ptr<dsp::VFOSplitterBlock> splitter_vfo;
            std::map<std::string, std::shared_ptr<satdump::pipeline::LivePipeline>> all_pipelines;
            bool radio_started = false;
            bool webserver_started = false;

            // Fork: stops the pipelines, radio and splitter started so far. Used when the run
            // ends and on any error once the radio runs (a VFO that cannot start, a bad setting): it
            // used to leave them running, and the program then crashed or waited for ever
            auto stop_running = [&]()
            {
                // Fork: release the shifters' output writers first. The splitter may be
                // blocked sending to a held consumer, backing up the radio callback too.
                for (auto &e : all_pipelines)
                    e.second->stop();
                if (radio_started)
                    satdump::stop_radio(source_ptr);
                radio_started = false;
                if (splitter_vfo)
                    splitter_vfo->stop();
                // Fork: and the frequency shifter add_vfo() started for each VFO, also one whose
                // pipeline did not start (del_vfo() does nothing for a VFO never added). Only the
                // normal end of the run did this; on an error they were destroyed with the
                // splitter while still running
                if (splitter_vfo)
                    for (auto cfg : multi_cfg.items())
                        splitter_vfo->del_vfo(cfg.key());
            };

            // Attempt to start the source and splitter.
            try
            {
                // Fork: start() may launch a receive callback before throwing; it still needs stop.
                radio_started = true;
                satdump::start_radio(source_ptr);
                splitter_vfo = std::make_unique<dsp::VFOSplitterBlock>(source_ptr->output_stream);
                splitter_vfo->set_main_enabled(false);
                splitter_vfo->start();
            }
            catch (satdump::RadioError &e)
            {
                stop_running();
                logger->error("Radio problem: could not start the radio: %s", e.what());
                return satdump::EXIT_RADIO;
            }
            catch (const std::exception &)
            {
                stop_running();
                // Fork: a DSP startup failure is a command failure (exit 2), after cleanup.
                throw;
            }
            catch (...)
            {
                stop_running();
                throw;
            }

            try
            {
                for (auto cfg : multi_cfg.items())
                {
                    double vfrequency = cfg.value()["frequency"];
                    std::string vpipeline = cfg.value()["pipeline"];
                    nlohmann::json vparams = cfg.value()["parameters"];

                    double final_shift = double(frequency) - vfrequency;

                    if (std::abs(final_shift) > (samplerate / 2))
                    {
                        logger->error("Frequency shift for VFO %s is outside of samplerate range!", cfg.key().c_str());
                        // Fork: unwind through the owners after stopping everything already
                        // started. exit(1) bypassed the radio, splitter and earlier VFOs.
                        stop_running();
                        return 1;
                    }

                    std::optional<satdump::pipeline::Pipeline> pipeline = satdump::pipeline::getPipelineFromID(vpipeline);
                    vparams["baseband_format"] = "cf32";
                    vparams["buffer_size"] = dsp::STREAM_BUFFER_SIZE; // This is required, as we WILL go over the (usually) default 8192 size
                    vparams["start_timestamp"] = (double)time(0);     // Some pipelines need this
                    vparams["samplerate"] = samplerate;

                    std::string path = output_file + "/" + cfg.key();
                    if (!std::filesystem::exists(path))
                        std::filesystem::create_directories(path);

                    std::shared_ptr<satdump::pipeline::LivePipeline> live_pipeline = std::make_shared<satdump::pipeline::LivePipeline>(pipeline.value(), vparams, path);

                    bool server_mode = vparams.contains("server_address") || vparams.contains("server_port");

                    splitter_vfo->add_vfo(cfg.key(), samplerate, final_shift);
                    live_pipeline->start(splitter_vfo->get_vfo_output(cfg.key()), live_thread_pool, server_mode);
                    splitter_vfo->set_vfo_enabled(cfg.key(), true);

                    all_pipelines.emplace(cfg.key(), live_pipeline);
                    // Fork: the VFO name (from the settings) is a %s argument; it was part of the format
                    logger->info("Added VFO for %s at %f", cfg.key().c_str(), final_shift);
#ifdef SATDUMP_TEST_LIVE_RUNNING
                    // Fork: test sources can establish backpressure before the next VFO.
                    satdump::eventBus->fire_event(fork_test::VfoStartedEvent{});
#endif
                }

                // If requested, boot up webserver
                if (parameters.contains("http_server"))
                {
                    // if (!webserver_already_set)
                    webserver::handle_callback = [&all_pipelines]()
                    {
                        nlohmann::json stats;
                        for (auto &e : all_pipelines)
                        {
                            stats[e.first] = e.second->getModulesStats();
                        }
                        return stats.dump(4);
                    };
                    if (!start_webserver(parameters))
                    {
                        stop_running();
                        return 1;
                    }
                    webserver_started = true;
                }

                // Now, we wait
                uint64_t start_time = time(0);
                while (1)
                {
                    if (timeout > 0)
                    {
                        uint64_t elapsed_time = time(0) - start_time;
                        if (elapsed_time >= timeout)
                        {
                            // Fork: PRIu64 for a uint64_t (was the wrong printf type for it)
                            logger->warn("Timeout is over! (%" PRIu64 "s >= %" PRIu64 "s) Stopping.", elapsed_time, timeout);
                            break;
                        }

                        // live_pipeline->stats["timeout_left"] = timeout - elapsed_time;
                    }

                    if (satdump::cli::stop_requested())
                    {
                        logger->warn("Signal Received. Stopping.");
                        break;
                    }

                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }

                if (webserver_started)
                    webserver::stop();
                webserver_started = false;

                // Stop cleanly
                stop_running();
                for (auto &e : all_pipelines)
                    logger->info("Stopped VFO %s", e.first.c_str()); // Fork: the name is a %s argument
            }
            catch (...)
            {
                if (webserver_started)
                    webserver::stop();
                stop_running();
                throw;
            }

            std::map<std::string, satdump::pipeline::LivePipeline *> vfo_pipelines;
            for (auto &e : all_pipelines)
                vfo_pipelines.emplace(e.first, e.second.get());
            throw_if_failed(vfo_pipelines);
        }
        else
        {
            // Get pipeline
            std::optional<satdump::pipeline::Pipeline> pipeline = satdump::pipeline::getPipelineFromID(downlink_pipeline);

            if (!pipeline.has_value())
            {
                logger->critical("Pipeline " + downlink_pipeline + " does not exist!");
                return 1;
            }

            // Fork: the thread pool is made before the pipeline, so it goes after it. It went first, and
            // waited for ever on the modules still running in it when an error left this block early
            ctpl::thread_pool live_thread_pool(8);

            // Init pipeline
            parameters["baseband_format"] = "cf32";
            parameters["buffer_size"] = dsp::STREAM_BUFFER_SIZE; // This is required, as we WILL go over the (usually) default 8192 size
            parameters["start_timestamp"] = (double)time(0);     // Some pipelines need this
            std::unique_ptr<satdump::pipeline::LivePipeline> live_pipeline = std::make_unique<satdump::pipeline::LivePipeline>(pipeline.value(), parameters, output_file);

            bool server_mode = parameters.contains("server_address") || parameters.contains("server_port");

            std::unique_ptr<dsp::SplitterBlock> splitter;
            std::unique_ptr<dsp::FFTPanBlock> fft;
            bool webserver_already_set = false;
            bool radio_started = false;
            bool webserver_started = false;

            nlohmann::json live_pipeline_stats;

            // Fork: stops the radio, the FFT blocks and the pipeline, whichever were started. Used when
            // the run ends and on any error once the radio runs: an error used to leave them running,
            // so the FFT blocks were destroyed while still running and the program waited for ever
            auto stop_running = [&]()
            {
                if (radio_started)
                    satdump::stop_radio(source_ptr);
                radio_started = false;
                if (splitter)
                    splitter->stop();
                if (fft)
                    fft->stop();
                live_pipeline->stop();
            };

            // Attempt to start the source and pipeline
            try
            {
                // Fork: start() may launch a receive callback before throwing; it still needs stop.
                radio_started = true;
                satdump::start_radio(source_ptr);

                std::shared_ptr<dsp::stream<complex_t>> final_stream = source_ptr->output_stream;

                // Optional FFT
                if (parameters.contains("fft_enable"))
                {
                    int fft_size = parameters.contains("fft_size") ? parameters["fft_size"].get<int>() : 512;
                    int fft_rate = parameters.contains("fft_rate") ? parameters["fft_rate"].get<int>() : 30;

                    splitter = std::make_unique<dsp::SplitterBlock>(source_ptr->output_stream);
                    splitter->add_output("fft");
                    splitter->set_enabled("fft", true);
                    final_stream = splitter->output_stream;
                    fft = std::make_unique<dsp::FFTPanBlock>(splitter->get_output("fft"));
                    fft->set_fft_settings(fft_size, samplerate, fft_rate);
                    if (parameters.contains("fft_avgn"))
                        fft->avg_num = parameters["fft_avgn"].get<float>();
                    // Fork: the FFT must be ready before the splitter publishes to it.
                    fft->start();
                    splitter->start();

                    webserver::handle_callback = [&live_pipeline, &fft, fft_size, &live_pipeline_stats]()
                    {
                        live_pipeline_stats = live_pipeline->getModulesStats();
                        for (int i = 0; i < fft_size; i++)
                            live_pipeline_stats["fft_values"][i] = fft->output_stream->writeBuf[i];
                        return live_pipeline_stats.dump(4);
                    };

                    webserver_already_set = true;
                }

                live_pipeline->start(final_stream, live_thread_pool, server_mode);
            }
            catch (satdump::RadioError &e)
            {
                stop_running();
                logger->error("Radio problem: could not start the radio: %s", e.what());
                return satdump::EXIT_RADIO;
            }
            catch (const std::exception &)
            {
                stop_running();
                // Fork: report operational startup failures through the command's exit-2 path.
                throw;
            }
            catch (...)
            {
                stop_running();
                throw;
            }

            try
            {
                // If requested, boot up webserver
                if (parameters.contains("http_server"))
                {
                    if (!webserver_already_set)
                        webserver::handle_callback = [&live_pipeline]() { return live_pipeline->getModulesStats().dump(4); };
                    if (!start_webserver(parameters))
                    {
                        stop_running();
                        return 1;
                    }
                    webserver_started = true;
                }

#ifdef SATDUMP_TEST_LIVE_RUNNING
                // Fork: the test plugin can throw on this thread with everything running.
                satdump::eventBus->fire_event(fork_test::LiveRunningEvent{});
#endif

                // Now, we wait
                uint64_t start_time = time(0);
                while (1)
                {
                    if (timeout > 0)
                    {
                        uint64_t elapsed_time = time(0) - start_time;
                        if (elapsed_time >= timeout)
                        {
                            // Fork: PRIu64 for a uint64_t (was the wrong printf type for it)
                            logger->warn("Timeout is over! (%" PRIu64 "s >= %" PRIu64 "s) Stopping.", elapsed_time, timeout);
                            break;
                        }

                        live_pipeline_stats["timeout_left"] = timeout - elapsed_time;
                    }

                    if (satdump::cli::stop_requested())
                    {
                        logger->warn("Signal Received. Stopping.");
                        break;
                    }

                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }

                // Stop cleanly
                stop_running();

                if ((parameters.contains("finish_processing") ? parameters["finish_processing"].get<bool>() : false) && !server_mode)
                {
                    if (live_pipeline->getOutputFile().size() > 0 || std::filesystem::exists(output_file + "/dataset.json"))
                    {
                        std::optional<satdump::pipeline::Pipeline> pipeline = satdump::pipeline::getPipelineFromID(downlink_pipeline);
                        std::string input_file = live_pipeline->getOutputFile().size() > 0 ? live_pipeline->getOutputFile() : (output_file + "/dataset.json");
                        int start_level = pipeline->live_cfg.normal_live[pipeline->live_cfg.normal_live.size() - 1];
                        std::string input_level = pipeline->steps[start_level].level;

                        logger->critical(input_level);

                        try
                        {
                            pipeline.value().run(input_file, output_file, parameters, input_level);
                        }
                        catch (std::exception &e)
                        {
                            logger->error("Fatal error running pipeline : " + std::string(e.what()));
                        }
                    }
                }
            }
            catch (...)
            {
                // Fork: an error in this block stops whatever still runs (the radio, the FFT blocks,
                // the pipeline) before it is reported. Upstream, an error while they ran (--http_server
                // given as a number threw here) left them running, and the program waited for ever.
                // Fork: the fault test module exercises this with the radio, FFT and pipeline
                // running. An error after the stop, such as --finish_processing given as a number,
                // comes here too; stopping again then does nothing
                stop_running();
                if (webserver_started)
                    webserver::stop();
                throw;
            }

            if (webserver_started)
                webserver::stop();

            throw_if_failed({{"", live_pipeline.get()}});
        }
    }

    return 0;
}