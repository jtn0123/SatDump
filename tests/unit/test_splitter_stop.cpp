// Fork: a splitter must stop with any output unread, including consumers stopped first.
#include "common/dsp/path/splitter.h"
#include "doctest.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <thread>

TEST_CASE("splitter wakes every unread output before joining")
{
    for (int kind = 0; kind < 4; kind++)
    {
        auto input = std::make_shared<dsp::stream<complex_t>>();
        dsp::SplitterBlock splitter(input);
        splitter.set_main_enabled(kind == 0);
        std::shared_ptr<dsp::stream<complex_t>> destination = splitter.output_stream;
        if (kind == 1 || kind == 3)
        {
            splitter.add_output("fft");
            splitter.set_enabled("fft", true);
            destination = splitter.get_output("fft");
        }
        if (kind == 2)
        {
            splitter.add_vfo("vfo", 6000000, 0);
            splitter.set_vfo_enabled("vfo", true);
            destination = splitter.get_vfo_output("vfo");
        }
        // Fork: a reader stopped first does not wake the writer. This models downstream-first
        // pipeline shutdown as well as a consumer whose start() threw.
        destination->stopReader();
        splitter.start();
        for (int i = 0; i < 3; i++)
        {
            input->writeBuf[0] = complex_t(float(i));
            REQUIRE(input->swap(1));
        }
        if (kind == 3)
            splitter.del_output("fft");
        // Fork: swap three can complete only after work() flushed input two. Its destination
        // still holds buffer one, so publication of buffer two cannot complete without stop().
        std::atomic<bool> done{false};
        std::atomic<bool> failed{false};
        std::thread stopper([&]()
        {
            try { splitter.stop(); }
            catch (const std::exception &) { failed = true; }
            catch (...) { failed = true; }
            done = true;
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (!done && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (!done)
        {
            std::fprintf(stderr, "splitter stop hung on output kind %d\n", kind);
            std::fflush(stderr);
            std::_Exit(1);
        }
        stopper.join();
        CHECK_FALSE(failed);
    }
}
