#include "autotrack.h"
#include "common/utils.h"
#include "logger.h"
#include "utils/thread_priority.h"
#include "utils/time.h"

void AutoTrackApp::start_processing()
{
    if (is_processing)
        return;

    // Fork: the lock is let go on every way out. An error that was not a runtime_error left it
    // taken, and the next pass then waited on it for ever
    std::lock_guard<std::mutex> lock(live_pipeline_mtx);
    logger->trace("Start pipeline...");
    pipeline_params["samplerate"] = get_samplerate();
    pipeline_params["baseband_format"] = "cf32";
    pipeline_params["buffer_size"] = dsp::STREAM_BUFFER_SIZE; // This is required, as we WILL go over the (usually) default 8192 size
    pipeline_params["start_timestamp"] = (double)time(0);     // Some pipelines need this

    try
    {
        pipeline_output_dir = prepareAutomatedPipelineFolder(time(0), source_ptr->d_frequency, selected_pipeline.id, d_output_folder);
        live_pipeline = std::make_unique<satdump::pipeline::LivePipeline>(selected_pipeline, pipeline_params, pipeline_output_dir);
        splitter->reset_output("live");
        live_pipeline->start(splitter->get_output("live"), main_thread_pool);
        splitter->set_enabled("live", true);

        is_processing = true;
    }
    catch (const std::exception &e) // Fork: any error, not only runtime_error (others left the lock taken)
    {
        logger->error("Could not start the live pipeline: %s", e.what());
    }
    catch (...)
    {
        logger->error("Could not start the live pipeline: unknown error");
    }
}

void AutoTrackApp::stop_processing()
{
    if (is_processing)
    {
        std::lock_guard<std::mutex> lock(live_pipeline_mtx); // Fork: also let go when something below throws
        logger->trace("Stop pipeline...");
        is_processing = false;
        splitter->set_enabled("live", false);
        live_pipeline->stop();

        if (d_settings.contains("finish_processing") && d_settings["finish_processing"].get<bool>() && live_pipeline->getOutputFile().size() > 0)
        {
            std::string input_file = live_pipeline->getOutputFile();
            satdump::pipeline::Pipeline selected_pipeline_ = selected_pipeline;
            std::string pipeline_output_dir_ = pipeline_output_dir;
            nlohmann::json pipeline_params_ = pipeline_params;
            auto fun = [selected_pipeline_, pipeline_output_dir_, input_file, pipeline_params_](int)
            {
                satdump::setLowestThreadPriority(); // TODOREWORK namespace remove
                // Fork: an error here was kept in a result nobody reads, so it was lost without a word
                try
                {
                    satdump::pipeline::Pipeline pipeline = selected_pipeline_;
                    int start_level = pipeline.live_cfg.normal_live[pipeline.live_cfg.normal_live.size() - 1];
                    std::string input_level = pipeline.steps[start_level].level;
                    pipeline.run(input_file, pipeline_output_dir_, pipeline_params_, input_level);
                    logger->info("Pipeline Processing Done!");
                }
                catch (const std::exception &e)
                {
                    logger->error("Processing after the pass failed: %s", e.what());
                }
                catch (...)
                {
                    logger->error("Processing after the pass failed with an unknown error");
                }
            };
            main_thread_pool.push(fun);
        }

        live_pipeline.reset();
    }
}

void AutoTrackApp::start_recording()
{
    splitter->set_enabled("record", true);

    std::string filename = d_output_folder + "/" + prepareBasebandFileName(satdump::getTime(), get_samplerate(), frequency_hz);
    std::string recorder_filename = file_sink->start_recording(filename, get_samplerate());
    logger->info("Recording to " + recorder_filename);
    is_recording = true;
}

void AutoTrackApp::stop_recording()
{
    if (is_recording)
    {
        file_sink->stop_recording();
        splitter->set_enabled("record", false);
        //  recorder_filename = "";
        is_recording = false;
    }
}