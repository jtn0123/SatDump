// Fork: observe real socket operations in the actual module, using only ephemeral loopback ports.
#include "pipeline/modules/network/module_network_client.h"
#include "doctest.h"
#include <nng/nng.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <cstring>
namespace
{
    std::atomic<int> receive_calls{0}, bound_port{0};
    size_t receive_limit = 0;
    int observed_set_size(nng_socket socket, const char *name, size_t value)
    {
        if (std::strcmp(name, NNG_OPT_RECVMAXSZ) == 0) receive_limit = value;
        return nng_socket_set_size(socket, name, value);
    }
    ssize_t observed_recvfrom(int fd, void *buf, size_t len, int flags, sockaddr *address, socklen_t *size)
    {
        sockaddr_in local{};
        socklen_t length = sizeof(local);
        if (getsockname(fd, reinterpret_cast<sockaddr *>(&local), &length) == 0)
            bound_port = ntohs(local.sin_port);
        receive_calls++;
        return recvfrom(fd, buf, len, flags, address, size);
    }
    template <typename F> bool within(F ready)
    {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!ready() && std::chrono::steady_clock::now() < end)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return ready();
    }
}
#define recvfrom observed_recvfrom
#define nng_socket_set_size observed_set_size
#include "../../src-core/pipeline/modules/network/module_network_client.cpp"
#undef nng_socket_set_size
#undef recvfrom
using satdump::pipeline::network::NetworkClientModule;
TEST_CASE("network shutdown: NNG bounds each queued message before dialing")
{
    NetworkClientModule module("", "", {{"pkt_size", 17}, {"server_address", "127.0.0.1"}, {"server_port", 1}});
    module.input_active = false;
    receive_limit = 0;
    module.process();
    CHECK(receive_limit == 17);
}
TEST_CASE("network shutdown: silent UDP stops promptly after preserving normal datagrams")
{
    NetworkClientModule module("", "", {{"client_mode", "udp_listen"}, {"pkt_size", 16}, {"server_port", 0}});
    module.output_fifo = std::make_shared<dsp::RingBuffer<uint8_t>>(100);
    module.input_active = true;
    receive_calls = 0;
    bound_port = 0;
    std::atomic<bool> done{false}, failed{false};
    std::thread worker([&]
    {
        try { module.process(); }
        catch (const std::exception &) { failed = true; }
        catch (...) { failed = true; }
        done = true;
    });
    if (!within([] { return bound_port.load() != 0; })) std::_Exit(1);
    int sender = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(bound_port.load());
    const uint8_t bytes[] = {1, 2, 3};
    CHECK(sendto(sender, bytes, 3, 0, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 3);
    close(sender);
    if (!within([&] { return module.output_fifo->getReadable() == 3; })) std::_Exit(1);
    uint8_t got[3]{};
    CHECK(module.output_fifo->read(got, 3) == 3);
    CHECK(std::memcmp(got, bytes, 3) == 0);
    // Fork: a second recv proves the worker is waiting on a now-silent socket before stop.
    if (!within([] { return receive_calls.load() >= 2; })) std::_Exit(1);
    const auto stop = std::chrono::steady_clock::now();
    module.input_active = false;
    if (!within([&] { return done.load(); })) std::_Exit(1);
    worker.join();
    CHECK_FALSE(failed);
    CHECK(std::chrono::steady_clock::now() - stop < std::chrono::seconds(1));
}
