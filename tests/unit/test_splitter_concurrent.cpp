// Fork: cancelling a queued destination cannot depend on an earlier blocked destination.
// Exercise both splitters, deletion/reset, and both concurrent retirement orders without sleeps.
#include "common/dsp/path/splitter.h"
#include "common/dsp/path/splitter_vfo.h"
#include "doctest.h"
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

namespace
{
    template <typename Future>
    void completed(Future &future, const char *what)
    {
        if (future.wait_for(std::chrono::seconds(15)) != std::future_status::ready)
        {
            std::fprintf(stderr, "concurrent splitter retirement hung: %s\n", what);
            std::fflush(stderr);
            std::_Exit(1);
        }
        future.get();
    }

    class NumberedReader
    {
        std::shared_ptr<dsp::stream<complex_t>> stream;
        std::mutex mutex;
        std::condition_variable changed;
        std::vector<int> numbers;
        std::exception_ptr error;
        std::thread worker;

    public:
        explicit NumberedReader(std::shared_ptr<dsp::stream<complex_t>> stream) : stream(stream)
        {
            worker = std::thread([this]()
            {
                try
                {
                    while (this->stream->read() > 0)
                    {
                        {
                            std::lock_guard<std::mutex> lock(mutex);
                            numbers.push_back(int(this->stream->readBuf[0].real));
                        }
                        this->stream->flush();
                        changed.notify_all();
                    }
                }
                catch (const std::exception &)
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    error = std::current_exception();
                    changed.notify_all();
                }
                catch (...)
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    error = std::current_exception();
                    changed.notify_all();
                }
            });
        }
        void expect(int count)
        {
            std::unique_lock<std::mutex> lock(mutex);
            if (!changed.wait_for(lock, std::chrono::seconds(15), [&]() { return error || int(numbers.size()) >= count; }))
            {
                std::fprintf(stderr, "surviving splitter output stopped at %zu of %d samples\n", numbers.size(), count);
                std::fflush(stderr);
                std::_Exit(1);
            }
            if (error)
                std::rethrow_exception(error);
            REQUIRE(int(numbers.size()) == count);
            for (int i = 0; i < count; i++)
                CHECK(numbers[i] == i);
        }
        void stop()
        {
            stream->stopReader();
            if (worker.joinable())
                worker.join();
        }
        ~NumberedReader() { stop(); }
    };

    template <typename Splitter>
    void exercise(bool vfo, bool reset, int order)
    {
        // Fork: put blocking stream calls in a bounded case, so a regression cannot hang ctest.
        auto case_run = std::async(std::launch::async, [=]()
        {
            auto input = std::make_shared<dsp::stream<complex_t>>();
            Splitter splitter(input);
            NumberedReader good(splitter.output_stream);
            auto add = [&](const std::string &id)
            {
                if constexpr (std::is_same_v<Splitter, dsp::VFOSplitterBlock>)
                {
                    splitter.add_vfo(id, 6000000, 0);
                    splitter.set_vfo_enabled(id, true);
                }
                else if (vfo)
                {
                    splitter.add_vfo(id, 6000000, 0);
                    splitter.set_vfo_enabled(id, true);
                }
                else
                {
                    splitter.add_output(id);
                    splitter.set_enabled(id, true);
                }
            };
            auto get = [&](const std::string &id)
            {
                if constexpr (std::is_same_v<Splitter, dsp::VFOSplitterBlock>)
                    return splitter.get_vfo_output(id);
                else
                    return vfo ? splitter.get_vfo_output(id) : splitter.get_output(id);
            };
            auto retire = [&](const std::string &id)
            {
                if constexpr (std::is_same_v<Splitter, dsp::VFOSplitterBlock>)
                {
                    if (reset) splitter.reset_vfo(id);
                    else splitter.del_vfo(id);
                }
                else if (vfo)
                {
                    if (reset) splitter.reset_vfo(id);
                    else splitter.del_vfo(id);
                }
                else
                {
                    if (reset) splitter.reset_output(id);
                    else splitter.del_output(id);
                }
            };
            auto send = [&](int first, int end)
            {
                for (int i = first; i < end; i++)
                {
                    input->writeBuf[0] = complex_t(float(i));
                    REQUIRE(input->swap(1));
                }
            };
            add("A");
            add("B");
            std::weak_ptr<dsp::stream<complex_t>> old_a = get("A"), old_b = get("B");
            splitter.start();
            // Fork: the dedicated VFO splitter has a shifter and an extra buffer per output.
            // Enough input fills every buffer; the main output confirms that the worker reached
            // the batch whose A is stalled and whose B is still queued.
            constexpr bool dedicated = std::is_same_v<Splitter, dsp::VFOSplitterBlock>;
            const int filled = dedicated ? 5 : 3;
            send(0, filled);
            good.expect(filled - 1);
            if (order == 0)
            {
                auto queued = std::async(std::launch::async, [&]() { retire("B"); });
                completed(queued, "queued B must finish while A remains blocked");
                auto active = std::async(std::launch::async, [&]() { retire("A"); });
                completed(active, "active A");
            }
            else
            {
                const std::string first = order == 1 ? "A" : "B";
                const std::string second = order == 1 ? "B" : "A";
                std::promise<void> entered;
                auto entered_future = entered.get_future();
                auto one = std::async(std::launch::async, [&]() { entered.set_value(); retire(first); });
                completed(entered_future, "first retirement starts");
                auto two = std::async(std::launch::async, [&]() { retire(second); });
                completed(one, "first concurrent retirement");
                completed(two, "second concurrent retirement");
            }
            // Fork: reset must leave the fresh output disabled; neither an old queued output
            // nor a fresh disabled one may stall the surviving numbered stream.
            send(filled, filled + 6);
            good.expect(filled + 6);
            CHECK(old_a.expired());
            CHECK(old_b.expired());
            // Fork: dedicated reset must also connect its replacement input to a fresh shifter.
            if (reset)
            {
                auto fresh = get("A");
                if constexpr (dedicated)
                    splitter.set_vfo_enabled("A", true);
                else if (vfo)
                    splitter.set_vfo_enabled("A", true);
                else
                    splitter.set_enabled("A", true);
                auto one_sample = std::async(std::launch::async, [&]()
                {
                    REQUIRE(fresh->read() == 1);
                    CHECK(fresh->readBuf[0].real == float(filled + 6));
                    fresh->flush();
                });
                send(filled + 6, filled + 7);
                completed(one_sample, "reset output receives again");
                good.expect(filled + 7);
                if constexpr (dedicated)
                {
                    splitter.del_vfo("A");
                    splitter.del_vfo("B");
                }
                else if (vfo)
                {
                    splitter.del_vfo("A");
                    splitter.del_vfo("B");
                }
                else
                {
                    splitter.del_output("A");
                    splitter.del_output("B");
                }
            }
            splitter.stop_tmp();
            good.stop();
        });
        completed(case_run, "complete numbered-stream case");
    }
}

TEST_CASE("concurrent splitter retirements do not wait on other outputs")
{
    for (bool vfo : {false, true})
        for (bool reset : {false, true})
            for (int order = 0; order < 3; order++)
                exercise<dsp::SplitterBlock>(vfo, reset, order);
}

TEST_CASE("concurrent frequency shifter retirements do not wait on other outputs")
{
    for (bool reset : {false, true})
        for (int order = 0; order < 3; order++)
            exercise<dsp::VFOSplitterBlock>(true, reset, order);
}
