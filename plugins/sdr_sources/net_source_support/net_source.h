#pragma once

#include "common/dsp_source_sink/dsp_sample_source.h"
#include "common/net/udp.h"
#include "common/widgets/notated_num.h"
#include "core/style.h"
#include "imgui/imgui.h"
#include "logger.h"
#include <thread>
#include <atomic>

#include <nng/nng.h>
#include <nng/protocol/pubsub0/sub.h>

class NetSource : public dsp::DSPSampleSource
{
protected:
    bool is_open = false, is_started = false;

    enum netsource_mode_t
    {
        MODE_UDP,
        MODE_NNGSUB,
    };

    netsource_mode_t mode = MODE_UDP;
    std::string address = "localhost";
    int port = 8877;

    std::shared_ptr<net::UDPServer> udp_server;

    nng_socket n_sock{}; // Fork: independent partial-start ownership.
    nng_dialer n_dialer{};
    bool socket_owned = false, dialer_owned = false;

    satdump::widgets::NotatedNum<uint64_t> current_samplerate = satdump::widgets::NotatedNum<uint64_t>("Samplerate##net", 0, "sps");

    std::string error;

protected:
    std::atomic<bool> should_run{false}; // Fork: worker lifetime is per start/stop.
    std::thread work_thread;
    void run_thread();

public:
    NetSource(dsp::SourceDescriptor source) : DSPSampleSource(source)
    {
        // Fork: create the worker only after its resources have been acquired.
    }

    ~NetSource()
    {
        stop();
        close();
    }

    void set_settings(nlohmann::json settings);
    nlohmann::json get_settings();

    void open();
    void start();
    void stop();
    void close();

    void set_frequency(uint64_t frequency);

    void drawControlUI();

    void set_samplerate(uint64_t samplerate);
    uint64_t get_samplerate();

    static std::string getID() { return "net_source"; }
    static std::shared_ptr<dsp::DSPSampleSource> getInstance(dsp::SourceDescriptor source) { return std::make_shared<NetSource>(source); }
    static std::vector<dsp::SourceDescriptor> getAvailableSources();
};
