#include "net_source.h"
#include <cerrno>
#include "i18n.h"
#include "imgui/imgui_stdlib.h"

void NetSource::set_settings(nlohmann::json settings)
{
    d_settings = settings;

    if (d_settings.contains("mode"))
    {
        if (d_settings["mode"].get<std::string>() == "udp")
        {
            mode = MODE_UDP;
            port = getValueOrDefault(d_settings["port"], port);
        }
        else if (d_settings["mode"].get<std::string>() == "nng_sub")
        {
            mode = MODE_NNGSUB;
            address = getValueOrDefault(d_settings["address"], address);
            port = getValueOrDefault(d_settings["port"], port);
        }
    }
}

nlohmann::json NetSource::get_settings()
{

    if (mode == MODE_UDP)
    {
        d_settings["mode"] = "udp";
        d_settings["port"] = port;
    }
    else if (mode == MODE_NNGSUB)
    {
        d_settings["mode"] = "nng_sub";
        d_settings["address"] = address;
        d_settings["port"] = port;
    }

    return d_settings;
}

void NetSource::open()
{
    // Nothing to do
    is_open = true;
}

void NetSource::run_thread()
{
    // Fork: stop joins this worker before releasing its socket or stream.
    try
    {
        while (should_run)
        {
            if (mode == MODE_UDP)
            {
                int bytes;
                try { bytes = udp_server->recv((uint8_t *)output_stream->writeBuf, 8192 * sizeof(complex_t)); }
                catch (const std::exception &)
                {
                    if (!should_run)
                        continue;
#ifdef _WIN32
                    if (WSAGetLastError() == WSAETIMEDOUT || WSAGetLastError() == WSAEWOULDBLOCK)
                        continue;
#else
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                        continue;
#endif
                    throw;
                }
                if (bytes > 0)
                    output_stream->swap(bytes / sizeof(complex_t));
            }
            else
            {
                size_t size = dsp::STREAM_BUFFER_SIZE * sizeof(complex_t);
                int result = nng_recv(n_sock, output_stream->writeBuf, &size, 0);
                if (result == 0)
                    output_stream->swap(size / sizeof(complex_t));
                else if (result != NNG_ETIMEDOUT && result != NNG_ECLOSED)
                    throw std::runtime_error(nng_strerror(result));
            }
        }
    }
    catch (const std::exception &e) { logger->error("Network source worker failed: %s", e.what()); }
    catch (...) { logger->error("Network source worker failed: unknown exception"); }
    should_run = false;
}

void NetSource::start()
{
    if (mode == MODE_UDP)
    {
        udp_server = std::make_shared<net::UDPServer>(port);
        udp_server->enableTimeout(); // Fork: bound the receive wait before joining.
    }
    else if (mode == MODE_NNGSUB)
    {
        logger->info("Opening TCP socket on " + std::string("tcp://" + address + ":" + std::to_string(port)));
        // Fork: record each acquired handle before any following call can fail.
        auto check = [](int result) { if (result) throw std::runtime_error(nng_strerror(result)); };
        check(nng_sub0_open_raw(&n_sock));
        socket_owned = true;
        check(nng_socket_set_ms(n_sock, NNG_OPT_RECVTIMEO, 100));
        check(nng_dialer_create(&n_dialer, n_sock, std::string("tcp://" + address + ":" + std::to_string(port)).c_str()));
        dialer_owned = true;
        check(nng_dialer_start(n_dialer, 0));
    }

    DSPSampleSource::start();

    set_frequency(d_frequency);

    should_run = true;
    work_thread = std::thread(&NetSource::run_thread, this);
    is_started = true;
}

void NetSource::stop()
{
    // Fork: stop the worker before releasing resources it may still be using.
    should_run = false;
    if (output_stream)
        output_stream->stopWriter();
    if (dialer_owned)
        nng_dialer_close(n_dialer);
    dialer_owned = false;
    if (socket_owned)
        nng_close(n_sock);
    socket_owned = false;
    if (work_thread.joinable())
        work_thread.join();
    udp_server.reset();
    if (output_stream)
        output_stream->flush();
    is_started = false;
}

void NetSource::close() { is_open = false; }

void NetSource::set_frequency(uint64_t frequency)
{
    // if (is_open && is_connected)
    // {
    //     client->setFrequency(frequency);
    //     logger->debug("Set SDR++ Server frequency to %d", frequency);
    // }
    DSPSampleSource::set_frequency(frequency);
}

void NetSource::drawControlUI()
{
    if (is_started)
        style::beginDisabled();

    current_samplerate.draw();

    if (ImGui::RadioButton(_("UDP##netsource"), mode == MODE_UDP))
        mode = MODE_UDP;
    if (ImGui::RadioButton(_("NNG Sub##netsource"), mode == MODE_NNGSUB))
        mode = MODE_NNGSUB;

    if (mode == MODE_UDP)
    {
        ImGui::InputInt(_("Port"), &port);
    }
    else if (mode == MODE_NNGSUB)
    {
        ImGui::InputText(_("Address"), &address);
        ImGui::InputInt(_("Port"), &port);
    }

    if (is_started)
        style::endDisabled();
}

void NetSource::set_samplerate(uint64_t samplerate) { current_samplerate.set(samplerate); }

uint64_t NetSource::get_samplerate() { return current_samplerate.get(); }

std::vector<dsp::SourceDescriptor> NetSource::getAvailableSources()
{
    std::vector<dsp::SourceDescriptor> results;

    results.push_back({"net_source", _("Network Source"), "0", false});

    return results;
}
