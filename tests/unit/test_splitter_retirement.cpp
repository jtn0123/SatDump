// Fork: retiring one blocked destination must not freeze any surviving output. Disabling a
// retained destination is a publication barrier, as upstream callers expect before stopping it.
#include "common/dsp/path/splitter.h"
#include "doctest.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;

    bool within(const std::function<bool()> &condition, int seconds = 15)
    {
        const auto deadline = Clock::now() + std::chrono::seconds(seconds);
        while (!condition() && Clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return condition();
    }

    void bounded(const char *name, const std::function<void()> &action)
    {
        std::atomic<bool> done{false};
        std::exception_ptr error;
        std::thread worker([&]()
        {
            try { action(); }
            catch (const std::exception &) { error = std::current_exception(); }
            catch (...) { error = std::current_exception(); }
            done = true;
        });
        if (!within([&]() { return done.load(); }, 20))
        {
            std::fprintf(stderr, "splitter retirement hung: %s\n", name);
            std::fflush(stderr);
            std::_Exit(1);
        }
        worker.join();
        if (error)
            std::rethrow_exception(error);
    }

    struct Reader
    {
        std::shared_ptr<dsp::stream<complex_t>> stream;
        std::atomic<bool> running{true}, release{true}, entered{false}, failed{false};
        std::atomic<int> count{0};
        std::mutex mutex;
        std::vector<int> numbers;
        std::thread thread;

        explicit Reader(std::shared_ptr<dsp::stream<complex_t>> stream, bool gated = false) : stream(stream)
        {
            release = !gated;
            thread = std::thread([this]()
            {
                try
                {
                    while (running)
                    {
                        const int n = this->stream->read();
                        if (n > 0)
                        {
                            entered = true;
                            while (running && !release)
                                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                            {
                                std::lock_guard<std::mutex> lock(mutex);
                                numbers.push_back(int(this->stream->readBuf[0].real));
                            }
                            this->stream->flush();
                            count++;
                        }
                    }
                }
                catch (const std::exception &) { failed = true; }
                catch (...) { failed = true; }
            });
        }
        void stop()
        {
            running = false;
            release = true;
            stream->stopReader();
            if (thread.joinable())
                thread.join();
        }
        ~Reader() { stop(); }
        std::vector<int> samples()
        {
            std::lock_guard<std::mutex> lock(mutex);
            return numbers;
        }
    };

    void send(const std::shared_ptr<dsp::stream<complex_t>> &input, int first, int end)
    {
        for (int i = first; i < end; i++)
        {
            input->writeBuf[0] = complex_t(float(i));
            REQUIRE(input->swap(1));
        }
    }
}

TEST_CASE("retiring a blocked output lets surviving numbered samples continue")
{
    for (int kind = 0; kind < 4; kind++)
        bounded("delete/reset blocked destination", [kind]()
        {
            auto input = std::make_shared<dsp::stream<complex_t>>();
            dsp::SplitterBlock splitter(input);
            Reader good(splitter.output_stream);
            const bool vfo = kind % 2;
            if (vfo)
            {
                splitter.add_vfo("retired", 6000000, 0);
                splitter.set_vfo_enabled("retired", true);
            }
            else
            {
                splitter.add_output("retired");
                splitter.set_enabled("retired", true);
            }
            auto retired = vfo ? splitter.get_vfo_output("retired") : splitter.get_output("retired");
            std::weak_ptr<dsp::stream<complex_t>> old = retired;
            splitter.start();
            // Fork: the first destination buffer remains unread. Input three proves work has
            // flushed input two and is publishing its second buffer into that full destination.
            send(input, 0, 3);
            retired.reset();
            if (kind < 2)
            {
                if (vfo) splitter.del_vfo("retired");
                else splitter.del_output("retired");
            }
            else
            {
                if (vfo) splitter.reset_vfo("retired");
                else splitter.reset_output("retired");
            }
            send(input, 3, 9);
            CHECK(within([&]() { return good.count == 9; }));
            CHECK(within([&]() { return old.expired(); }));
            CHECK(good.samples() == std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7, 8});
            // Fork: test progress and release of the old stream BEFORE any full stop. A reset
            // keeps the output disabled; delete/add and reset both supply usable fresh streams.
            if (kind < 2)
            {
                if (vfo) splitter.add_vfo("retired", 6000000, 0);
                else splitter.add_output("retired");
            }
            Reader fresh(vfo ? splitter.get_vfo_output("retired") : splitter.get_output("retired"));
            if (vfo) splitter.set_vfo_enabled("retired", true);
            else splitter.set_enabled("retired", true);
            send(input, 9, 12);
            CHECK(within([&]() { return fresh.count == 3 && good.count == 12; }));
            CHECK(fresh.samples() == std::vector<int>{9, 10, 11});
            if (vfo) splitter.set_vfo_enabled("retired", false);
            else splitter.set_enabled("retired", false);
            fresh.stop();
            splitter.stop_tmp();
            good.stop();
            CHECK_FALSE(good.failed);
            CHECK_FALSE(fresh.failed);
        });
}

TEST_CASE("disable waits for publication and reenable preserves the retained stream")
{
    for (int kind = 0; kind < 3; kind++)
        bounded("disable/re-enable retained destination", [kind]()
        {
            auto input = std::make_shared<dsp::stream<complex_t>>();
            dsp::SplitterBlock splitter(input);
            splitter.add_output("good");
            splitter.set_enabled("good", true);
            Reader good(splitter.get_output("good"));
            auto set_enabled = [&](bool enable)
            {
                if (kind == 0) splitter.set_main_enabled(enable);
                else if (kind == 1) splitter.set_enabled("retained", enable);
                else splitter.set_vfo_enabled("retained", enable);
            };
            if (kind == 1) splitter.add_output("retained");
            if (kind == 2) splitter.add_vfo("retained", 6000000, 0);
            if (kind != 0) splitter.set_main_enabled(false);
            set_enabled(true);
            auto retained = kind == 0 ? splitter.output_stream : kind == 1 ? splitter.get_output("retained") : splitter.get_vfo_output("retained");
            Reader consumer(retained, true);
            splitter.start();
            send(input, 0, 3);
            REQUIRE(within([&]() { return consumer.entered.load(); }));
            std::atomic<bool> entered{false}, disabled{false};
            std::exception_ptr error;
            std::thread disable([&]()
            {
                try { entered = true; set_enabled(false); disabled = true; }
                catch (const std::exception &) { error = std::current_exception(); }
                catch (...) { error = std::current_exception(); }
            });
            REQUIRE(within([&]() { return entered.load(); }));
            // Fork: a correct disable cannot return while the consumer deliberately holds
            // buffer one. A generous observation window allows a broken setter to return.
            const bool premature = within([&]() { return disabled.load(); }, 1);
            consumer.release = true;
            disable.join();
            if (error) std::rethrow_exception(error);
            CHECK_FALSE(premature);
            CHECK(within([&]() { return consumer.count == 2; }));
            consumer.stop();
            send(input, 3, 9);
            CHECK(within([&]() { return good.count == 9; }));
            CHECK(good.samples() == std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7, 8});
            // Fork: upstream retained stream identity and did not permanently stop its writer
            // when disabled. Restart its reader only after the previous reader has joined.
            retained->clearReadStop();
            Reader resumed(retained);
            set_enabled(true);
            send(input, 9, 12);
            CHECK(within([&]() { return resumed.count == 3 && good.count == 12; }));
            CHECK(resumed.samples() == std::vector<int>{9, 10, 11});
            set_enabled(false);
            resumed.stop();
            splitter.stop_tmp();
            good.stop();
            CHECK_FALSE(consumer.failed);
            CHECK_FALSE(resumed.failed);
        });
}
