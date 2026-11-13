// Fork: a real NNG publisher for CLI payload tests, never installed. It waits for a
// subscriber before sending and stays connected until the test has checked its output.
#include <nng/nng.h>
#include <nng/protocol/pubsub0/pub.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
    void check(int result)
    {
        if (result != 0)
            throw std::runtime_error(nng_strerror(result));
    }

    void connected(nng_pipe, nng_pipe_ev, void *state)
    {
        static_cast<std::atomic<bool> *>(state)->store(true);
    }
}

int main(int argc, char **argv)
{
    try
    {
        if (argc != 2)
            throw std::runtime_error("publisher needs a work folder");
        const std::filesystem::path work(argv[1]);
        std::atomic<bool> ready{false};
        nng_socket socket;
        check(nng_pub0_open(&socket));
        struct Close
        {
            nng_socket socket;
            ~Close() { nng_close(socket); }
        } close{socket};
        // Fork: queue the entire finite batch even if the receiver is scheduled late.
        check(nng_socket_set_int(socket, NNG_OPT_SENDBUF, 64));
        check(nng_pipe_notify(socket, NNG_PIPE_EV_ADD_POST, connected, &ready));
        nng_listener listener;
        check(nng_listen(socket, "tcp://127.0.0.1:0", &listener, 0));
        int port = 0;
        check(nng_listener_get_int(listener, NNG_OPT_TCP_BOUND_PORT, &port));
        std::cout << port << std::endl;

        std::vector<std::vector<unsigned char>> packets(12, std::vector<unsigned char>(1024));
        std::ofstream expected(work / "expected.bin", std::ios::binary);
        for (size_t index = 0; index < packets.size(); index++)
        {
            for (size_t offset = 0; offset < packets[index].size(); offset++)
                packets[index][offset] = (index * 37 + offset) % 256;
            expected.write(reinterpret_cast<const char *>(packets[index].data()), packets[index].size());
        }
        expected.close();
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (!ready && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (!ready)
            throw std::runtime_error("subscriber did not connect");

        auto send = [&](std::vector<unsigned char> message)
        { check(nng_send(socket, message.data(), message.size(), 0)); };
        // Fork: invalid sizes between valid groups expose truncation and stale bytes.
        // The final valid sentinel group proves the receiver continued after them.
        for (size_t index = 0; index < packets.size(); index++)
        {
            if (index == 4)
                send(std::vector<unsigned char>(1023, 's'));
            if (index == 8)
            {
                send(std::vector<unsigned char>(1025, 'o'));
                send(std::vector<unsigned char>(16384, 'L'));
            }
            send(packets[index]);
        }
        std::cout << "sent" << std::endl;
        deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
        while (!std::filesystem::exists(work / "release") && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (!std::filesystem::exists(work / "release"))
            throw std::runtime_error("test did not release publisher");
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Publisher failed: " << e.what() << std::endl;
        return 1;
    }
    catch (...)
    {
        std::cerr << "Publisher failed with unknown error" << std::endl;
        return 1;
    }
}
