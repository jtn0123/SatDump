// Fork: start() and stop() of one DSP block called at once on two threads, for both block
// classes: dsp::Block (the legacy one the demodulators use, src-core/common/dsp/block.h) and
// satdump::ndsp::Block (the new DSP, src-core/dsp/block.h). A live module's task starts its
// blocks in process() while the main thread may already be stopping the module (a SIGTERM while
// the live run starts), so the two can overlap. A legacy stop() cleared should_run, a start() on
// the other thread set it again and made a new thread, and stop() then joined that thread, which
// never ended: SatDump hung. In the new DSP a stop() could return while a start() went on to begin
// a run that nobody stopped, and both wrote and joined the same std::thread (a data race).
//
// Each class gets a test that holds one call at the point where the other used to slip in, which
// fails every time without the fix, and a stress test that races the two many times. A thread
// stuck in join() cannot be recovered, so every wait has a deadline that ends the test run.
#include "common/dsp/block.h"
#include "doctest.h"
#include "dsp/block.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace
{
    typedef std::chrono::steady_clock Clock;

    // Polls until cond() is true or the time is up
    bool within(double seconds, const std::function<bool()> &cond)
    {
        auto end = Clock::now() + std::chrono::duration<double>(seconds);
        while (Clock::now() < end)
        {
            if (cond())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return cond();
    }

    // Runs fn on a thread of its own and returns what it threw ("" if nothing). If fn has not
    // returned after `deadline` seconds the test run is ended, with `what` as the reason.
    std::string must_end_within(double deadline, const char *what, const std::function<void()> &fn)
    {
        std::atomic<bool> done{false};
        std::string error;
        std::thread th([&]
                       {
                           try
                           {
                               fn();
                           }
                           catch (const std::exception &e)
                           {
                               error = e.what();
                           }
                           catch (...)
                           {
                               error = "unknown exception";
                           }
                           done = true;
                       });
        if (!within(deadline, [&] { return done.load(); }))
        {
            std::fprintf(stderr, "test_block_start_stop: %s did not end within %.0f s (start() and stop() raced), giving up\n", what, deadline);
            std::fflush(stderr);
            std::_Exit(1);
        }
        th.join();
        return error;
    }

    // Runs fn on a new thread; what it throws goes to `error`
    std::thread caught_thread(std::string &error, std::function<void()> fn)
    {
        return std::thread([&error, fn]
                           {
                               try
                               {
                                   fn();
                               }
                               catch (const std::exception &e)
                               {
                                   error = e.what();
                               }
                               catch (...)
                               {
                                   error = "unknown exception";
                               }
                           });
    }

    // The sample type of the legacy test block. Its stream (below) lets a test hold
    // Block::stop() at the point where a start() on another thread used to slip in.
    struct GatedSample
    {
        float value;
    };
} // namespace

namespace dsp
{
    // Fork test seam: the stream of GatedSample, with the members dsp::Block uses. Block::stop()
    // calls stopReader() on its input after it cleared should_run and before it looks at the
    // block's thread; stopReader() first runs on_stop_reader, so a test can hold stop() there.
    // read() waits until the reader is stopped, then returns -1 at once, as a stopped stream does.
    template <>
    class stream<GatedSample>
    {
    private:
        std::mutex mtx;
        std::condition_variable cv;
        bool reader_stopped = false;

    public:
        std::function<void()> on_stop_reader;

        int read()
        {
            std::unique_lock<std::mutex> lck(mtx);
            cv.wait(lck, [this] { return reader_stopped; });
            return -1;
        }
        void flush() {}
        void stopReader()
        {
            if (on_stop_reader)
                on_stop_reader();
            {
                std::lock_guard<std::mutex> lck(mtx);
                reader_stopped = true;
            }
            cv.notify_all();
        }
        void clearReadStop()
        {
            std::lock_guard<std::mutex> lck(mtx);
            reader_stopped = false;
        }
        void stopWriter() {}
        void setWriterFailed() {}
        void clearWriterFailed() {}
        bool writerFailedAndEmpty() { return false; }
    };
} // namespace dsp

namespace
{
    // A legacy block like a demodulator's: it waits for input until its input is stopped. Started
    // again on a stopped input, it spins (work() returns at once) until stop(), as real ones do.
    class GatedBlock : public dsp::Block<GatedSample, GatedSample>
    {
    public:
        GatedBlock(std::shared_ptr<dsp::stream<GatedSample>> in) : dsp::Block<GatedSample, GatedSample>(in) {}
        bool running() { return should_run; }

    private:
        void work() override
        {
            if (input_stream->read() <= 0)
                input_stream->flush();
        }
    };

    // A new DSP block with no inputs or outputs, like a source: it runs until stop(true).
    // start() calls init() after it checked the block is stopped and before it starts the
    // thread; on_init lets a test hold start() there.
    class InitGatedBlock : public satdump::ndsp::Block
    {
    public:
        std::function<void()> on_init;

        InitGatedBlock() : satdump::ndsp::Block("init_gated_test") {}
        nlohmann::json get_cfg(std::string) { return nullptr; }
        cfg_res_t set_cfg(std::string, nlohmann::json) { return RES_OK; }
        bool running() { return is_work_running(); }

    protected:
        void init()
        {
            if (on_init)
                on_init();
        }

    private:
        bool work()
        {
            if (work_should_exit)
                return true;
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            return false;
        }
    };
} // namespace

TEST_CASE("block start/stop: legacy start() during a stop() on another thread waits for it")
{
    auto in = std::make_shared<dsp::stream<GatedSample>>();
    GatedBlock blk(in);

    // Hold stop() once it has cleared should_run, until start() has returned (or 0.3 s)
    std::atomic<bool> stop_held{false}, start_returned{false}, start_returned_while_held{false};
    in->on_stop_reader = [&]
    {
        stop_held = true;
        within(0.3, [&] { return start_returned.load(); });
        start_returned_while_held = start_returned.load();
    };

    std::string stop_error;
    bool stop_was_held = false;
    // Without the fix start() returned while stop() was held, and stop() then joined the
    // thread start() had made, which never ends: this did not end
    std::string error = must_end_within(10, "a legacy stop() raced by start()", [&]
                                        {
                                            std::thread stopper = caught_thread(stop_error, [&] { blk.stop(); });
                                            stop_was_held = within(5, [&] { return stop_held.load(); });
                                            blk.start();
                                            start_returned = true;
                                            stopper.join();
                                        });
    CHECK(error == "");
    CHECK(stop_error == "");
    CHECK(stop_was_held);
    CHECK_FALSE(start_returned_while_held); // start() waited for the stop() under way
    CHECK(blk.running());                   // ...and then started the block, which stop() stops
    in->on_stop_reader = nullptr;
    CHECK(must_end_within(10, "stopping the legacy test block", [&] { blk.stop(); }) == "");
    CHECK_FALSE(blk.running());
}

TEST_CASE("block start/stop: legacy start() and stop() raced many times always end")
{
    // stop() pauses 1 ms where a start() used to slip in, so the overlap is likely each time
    auto in = std::make_shared<dsp::stream<GatedSample>>();
    in->on_stop_reader = [] { std::this_thread::sleep_for(std::chrono::milliseconds(1)); };

    const int rounds = 200;
    int done = 0;
    std::string errors;
    std::string error = must_end_within(60, "legacy start() and stop() raced many times", [&]
                                        {
                                            for (int i = 0; i < rounds; i++)
                                            {
                                                in->clearReadStop();
                                                GatedBlock blk(in);
                                                std::atomic<int> ready{0};
                                                std::atomic<bool> go{false};
                                                std::string stop_error, start_error;
                                                std::thread stopper = caught_thread(stop_error, [&]
                                                                                    {
                                                                                        ready++;
                                                                                        while (!go)
                                                                                            std::this_thread::yield();
                                                                                        blk.stop();
                                                                                    });
                                                std::thread starter = caught_thread(start_error, [&]
                                                                                    {
                                                                                        ready++;
                                                                                        while (!go)
                                                                                            std::this_thread::yield();
                                                                                        blk.start();
                                                                                    });
                                                while (ready < 2)
                                                    std::this_thread::yield();
                                                go = true;
                                                stopper.join();
                                                starter.join();
                                                errors += stop_error + start_error;
                                                blk.stop(); // Whichever came first, the block now stops
                                                done++;
                                            }
                                        });
    CHECK(error == "");
    CHECK(errors == "");
    CHECK(done == rounds);
}

TEST_CASE("block start/stop: new DSP stop() during a start() on another thread stops the run it began")
{
    InitGatedBlock blk;

    // Hold start() once it has checked the block is stopped, until stop() was called, then
    // 0.1 s more: without the fix stop() returned in that time, and start() then began a run
    std::atomic<bool> start_held{false}, stop_called{false};
    blk.on_init = [&]
    {
        start_held = true;
        within(5, [&] { return stop_called.load(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    };

    std::string start_error;
    bool start_was_held = false;
    std::string error = must_end_within(10, "a new DSP start() raced by stop(true)", [&]
                                        {
                                            std::thread starter = caught_thread(start_error, [&] { blk.start(); });
                                            start_was_held = within(5, [&] { return start_held.load(); });
                                            stop_called = true;
                                            blk.stop(true);
                                            starter.join();
                                        });
    CHECK(error == "");
    CHECK(start_error == "");
    CHECK(start_was_held);
    CHECK_FALSE(blk.running()); // The stop() came after start() began, so that run is stopped too
    blk.on_init = nullptr;
    CHECK(must_end_within(10, "stopping the new DSP test block", [&] { blk.stop(true); }) == "");
}

TEST_CASE("block start/stop: new DSP start() and stop() raced many times always end")
{
    // start() pauses 1 ms where a stop() used to slip past it, so the overlap is likely each time
    InitGatedBlock blk;
    blk.on_init = [] { std::this_thread::sleep_for(std::chrono::milliseconds(1)); };

    const int rounds = 200;
    int done = 0;
    std::string errors;
    std::string error = must_end_within(60, "new DSP start() and stop() raced many times", [&]
                                        {
                                            for (int i = 0; i < rounds; i++)
                                            {
                                                std::atomic<int> ready{0};
                                                std::atomic<bool> go{false};
                                                std::string stop_error, start_error;
                                                std::thread stopper = caught_thread(stop_error, [&]
                                                                                    {
                                                                                        ready++;
                                                                                        while (!go)
                                                                                            std::this_thread::yield();
                                                                                        blk.stop(true);
                                                                                    });
                                                std::thread starter = caught_thread(start_error, [&]
                                                                                    {
                                                                                        ready++;
                                                                                        while (!go)
                                                                                            std::this_thread::yield();
                                                                                        blk.start();
                                                                                    });
                                                while (ready < 2)
                                                    std::this_thread::yield();
                                                go = true;
                                                stopper.join();
                                                starter.join();
                                                errors += stop_error + start_error;
                                                blk.stop(true); // Whichever came first, the block now stops
                                                done++;
                                            }
                                        });
    CHECK(error == "");
    CHECK(errors == "");
    CHECK(done == rounds);
    CHECK_FALSE(blk.running());
}
