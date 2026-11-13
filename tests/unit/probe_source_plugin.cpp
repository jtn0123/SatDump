// Fork: a radio driver for CLI tests only, never installed. Its type is "fork_probe".
// It sends no samples by default; produce_samples feeds five batches after a VFO starts.
// It lets the test watch how a live run starts:
//   - each time it is asked for its radios it adds a line to the file SATDUMP_TEST_PROBE_LISTED
//     names. A run for another radio type must not ask it: drivers are asked the same way, and
//     some of them search the network for radios when asked (UHD, PlutoSDR, SoapySDR)
//   - opening its radio adds a line to the file SATDUMP_TEST_PROBE_OPENING names, then waits
//     (60 s at most) for the file SATDUMP_TEST_PROBE_RELEASE names to exist, so the test can send
//     a signal while SatDump is still starting

#include "common/dsp_source_sink/dsp_sample_source.h"
#include "core/plugin.h"
#include "logger.h"
#include "live_running_event.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

namespace
{
    void mark(const char *variable)
    {
        const char *path = std::getenv(variable);
        if (path != nullptr)
            std::ofstream(path, std::ios::app) << "1\n";
    }

    class ProbeSource : public dsp::DSPSampleSource
    {
        bool started = false;
        uint64_t samplerate = 0;
        // Fork: optional samples make VFO backpressure reproducible without a real radio.
        struct Samples
        {
            std::atomic<bool> go{false}, stop{false}, failed{false};
            std::atomic<int> produced{0};
        };
        std::shared_ptr<Samples> samples;
        std::thread producer;

    public:
        explicit ProbeSource(dsp::SourceDescriptor source) : dsp::DSPSampleSource(source) {}

        // Fork: cleanup tests must see stop(), including on paths that used exit(1).
        ~ProbeSource()
        {
            if (started)
                logger->error("Fork probe radio destroyed while running");
            stop();
        }

        void open() override
        {
            mark("SATDUMP_TEST_PROBE_OPENING");
            const char *release = std::getenv("SATDUMP_TEST_PROBE_RELEASE");
            for (int i = 0; release != nullptr && i < 600 && !std::filesystem::exists(release); i++)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        void start() override
        {
            dsp::DSPSampleSource::start();
            started = true;
            logger->info("Fork probe radio started");
            if (!d_settings.value("produce_samples", false))
                return;
            samples = std::make_shared<Samples>();
            auto state = samples;
            auto output = output_stream;
            // Fork: no source samples precede the first enabled VFO. Five published batches
            // mean the splitter has reached the held branch's full buffer, even on one CPU.
            satdump::eventBus->register_handler<fork_test::VfoStartedEvent>([state](fork_test::VfoStartedEvent)
            {
                state->go = true;
                auto end = std::chrono::steady_clock::now() + std::chrono::seconds(20);
                while (state->produced < 5 && !state->failed && std::chrono::steady_clock::now() < end)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                if (state->produced < 5)
                    throw satdump_exception("probe did not establish VFO backpressure");
                logger->info("Fork probe VFO backpressure ready");
            });
            producer = std::thread([state, output]
            {
                try
                {
                    while (!state->go && !state->stop)
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    for (int batch = 0; batch < 5 && !state->stop; batch++)
                    {
                        for (int i = 0; i < 64; i++)
                            output->writeBuf[i] = complex_t(1, 0);
                        if (!output->swap(64))
                            break;
                        state->produced++;
                    }
                }
                catch (const std::exception &e)
                {
                    state->failed = true;
                    logger->error("Fork probe producer failed: %s", e.what());
                }
                catch (...)
                {
                    state->failed = true;
                    logger->error("Fork probe producer failed with unknown error");
                }
            });
        }

        void stop() override
        {
            if (started)
            {
                if (samples)
                {
                    samples->stop = true;
                    output_stream->stopWriter();
                    if (producer.joinable())
                        producer.join();
                }
                else
                    output_stream->flush();
                logger->info("Fork probe radio stopped");
            }
            started = false;
        }

        void close() override {}
        void drawControlUI() override {}
        void set_samplerate(uint64_t s) override { samplerate = s; }
        uint64_t get_samplerate() override { return samplerate; }
    };

    std::vector<dsp::SourceDescriptor> listProbeSources()
    {
        mark("SATDUMP_TEST_PROBE_LISTED");
        return {{"fork_probe", "Fork probe radio (test)", "0"}};
    }

    std::shared_ptr<dsp::DSPSampleSource> makeProbeSource(dsp::SourceDescriptor source) { return std::make_shared<ProbeSource>(source); }
} // namespace

class ForkProbeSourceTestPlugin : public satdump::Plugin
{
public:
    std::string getID() override { return "fork_probe_source_test"; }

    void init() override
    {
        // Fork: an optional startup barrier lets early_signals.sh stop before command dispatch.
        mark("SATDUMP_TEST_PROBE_INIT_READY");
        const char *release = std::getenv("SATDUMP_TEST_PROBE_INIT_RELEASE");
        for (int i = 0; release != nullptr && i < 600 && !std::filesystem::exists(release); i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        satdump::eventBus->register_handler<dsp::RegisterDSPSampleSourcesEvent>(registerSources);
    }

    static void registerSources(const dsp::RegisterDSPSampleSourcesEvent &evt) { evt.dsp_sources_registry.insert({"fork_probe", {makeProbeSource, listProbeSources}}); }
};

PLUGIN_LOADER(ForkProbeSourceTestPlugin)
