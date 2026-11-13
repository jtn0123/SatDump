// Fork test support: a radio driver for tests/cli/supervision.sh only, never installed. Its type is
// "fork_probe", with one radio that sends no samples. It lets the test watch how a live run starts:
//   - each time it is asked for its radios it adds a line to the file SATDUMP_TEST_PROBE_LISTED
//     names. A run for another radio type must not ask it: drivers are asked the same way, and
//     some of them search the network for radios when asked (UHD, PlutoSDR, SoapySDR)
//   - opening its radio adds a line to the file SATDUMP_TEST_PROBE_OPENING names, then waits
//     (60 s at most) for the file SATDUMP_TEST_PROBE_RELEASE names to exist, so the test can send
//     a signal while SatDump is still starting

#include "common/dsp_source_sink/dsp_sample_source.h"
#include "core/plugin.h"
#include "logger.h"
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

    public:
        explicit ProbeSource(dsp::SourceDescriptor source) : dsp::DSPSampleSource(source) {}

        // Fork: cleanup tests must see stop(), including on paths that used exit(1).
        ~ProbeSource()
        {
            if (started)
                logger->error("Fork probe radio destroyed while running");
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
        }

        void stop() override
        {
            if (started)
            {
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

    void init() override { satdump::eventBus->register_handler<dsp::RegisterDSPSampleSourcesEvent>(registerSources); }

    static void registerSources(const dsp::RegisterDSPSampleSourcesEvent &evt) { evt.dsp_sources_registry.insert({"fork_probe", {makeProbeSource, listProbeSources}}); }
};

PLUGIN_LOADER(ForkProbeSourceTestPlugin)
