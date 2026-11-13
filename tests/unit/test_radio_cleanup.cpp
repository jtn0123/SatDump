// Fork: real driver workers with in-process streams only; no network peer or radio.
#include "doctest.h"
#include "../../plugins/sdr_sources/sdrpp_server_support/sdrpp_server.h"
#include "../../plugins/sdr_sources/net_source_support/net_source.h"
#include <chrono>
#include <thread>

namespace
{
    class ConverterFixture : public SDRPPServerSource
    {
    public:
        ConverterFixture() : SDRPPServerSource({"sdrpp_server", "test", "0"}) {}
        void launch(bool fully_started)
        {
            DSPSampleSource::start();
            client_output_stream = std::make_shared<dsp::stream<uint8_t>>();
            convertShouldRun = true;
            convertThread = std::thread(&ConverterFixture::convertFunction, this);
            is_started = fully_started;
        }
        void stopped_input()
        {
            DSPSampleSource::start();
            client_output_stream = std::make_shared<dsp::stream<uint8_t>>();
            client_output_stream->stopReader();
            convertShouldRun = true;
            // A stopped read must return without parsing a negative packet length.
            convertFunction();
        }
    };
    class NetworkFixture : public NetSource
    {
    public:
        NetworkFixture() : NetSource({"net_source", "test", "0"}) { port = 0; set_frequency(0); }
        void partial_udp()
        {
            // Simulate an exception after acquisition but before is_started is set.
            udp_server = std::make_shared<net::UDPServer>(0);
        }
        bool owns_udp() const { return bool(udp_server); }
    };
}

TEST_CASE("SDR++ converter stop wakes an idle input after partial and complete startup")
{
    for (bool fully_started : {false, true})
    {
        ConverterFixture source;
        source.stop();
        source.stop();
        source.launch(fully_started);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        source.stop();
        source.stop();
    }
}

TEST_CASE("SDR++ converter does not parse a stopped read")
{
    ConverterFixture source;
    source.stopped_input();
    source.stop();
}

TEST_CASE("Network source releases partial UDP acquisition and joins an idle receiver")
{
    NetworkFixture source;
    source.stop();
    source.stop();
    source.partial_udp();
    REQUIRE(source.owns_udp());
    source.stop();
    CHECK_FALSE(source.owns_udp());
    for (int i = 0; i < 3; ++i)
    {
        source.start();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        source.stop();
        source.stop();
        CHECK_FALSE(source.owns_udp());
    }
}

#ifdef SATDUMP_TEST_RTLTCP
#include "../../plugins/sdr_sources/rtltcp_support/rtltcp.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

TEST_CASE("RTL-TCP stop cancels a silent in-process peer before joining")
{
    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(listener >= 0);
    struct CloseSocket { int fd; ~CloseSocket() { ::close(fd); } } close_listener{listener};
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
    socklen_t length = sizeof(address);
    REQUIRE(getsockname(listener, reinterpret_cast<sockaddr *>(&address), &length) == 0);
    REQUIRE(listen(listener, 1) == 0);
    RTLTCPSource source({"rtltcp", "test", "0"});
    source.stop();
    source.stop();
    source.open();
    source.set_frequency(1000000);
    source.set_settings({{"ip_address", "127.0.0.1"}, {"port", ntohs(address.sin_port)}});
    source.start();
    const int peer = accept(listener, nullptr, nullptr);
    REQUIRE(peer >= 0);
    CloseSocket close_peer{peer};
    // Leave the local connection silent: the driver's receiveData waits for samples.
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    source.stop();
    source.stop();
}
#endif
