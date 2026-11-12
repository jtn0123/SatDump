// DSP block lifetime and failures, for both block classes: satdump::ndsp::Block (the new DSP,
// src-core/dsp/block.h, also run through a Flowgraph) and dsp::Block (the legacy one used by
// the demodulators, src-core/common/dsp/block.h). An exception thrown by work() used to leave
// the block's thread and end the program through std::terminate; destroying a block that was
// still running did the same (a throw from a destructor). The tests build small chains of test
// blocks and check that a failure is logged, the blocks around it end (also through blocks
// with several inputs, which used to wait forever on the input that did not end), stop()
// returns quickly, and a command during which a block failed ends as failed. They also check
// that a setting a block cannot apply is logged instead of dropped. Every wait has a deadline,
// so a regression fails instead of hanging the test run.
#include "common/dsp/block.h"
#include "common/dsp/buffer.h"
#include "core/cli/cli.h"
#include "core/cli/subcommand.h"
#include "core/plugin.h"
#include "doctest.h"
#include "dsp/block.h"
#include "dsp/block_start.h"
#include "dsp/flowgraph/flowgraph.h"
#include "dsp/flowgraph/node_int.h"
#include "dsp/path/selector.h"
#include "dsp/path/splitter.h"
#include "dsp/utils/add.h"
#include "logger.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace
{
    using namespace satdump::ndsp;
    typedef std::chrono::steady_clock Clock;

    // Keeps the log lines written while it exists
    class LogCapture
    {
        struct Sink : public slog::LoggerSink
        {
            std::mutex mtx;
            std::vector<std::string> lines;
            void receive(slog::LogMsg log)
            {
                std::lock_guard<std::mutex> lg(mtx);
                lines.push_back(log.str);
            }
        };
        std::shared_ptr<Sink> sink = std::make_shared<Sink>();

    public:
        LogCapture()
        {
            // Until SatDump has started up, the logger keeps every line and hands them to each
            // new sink, so this capture would also see the lines of earlier tests. End that
            // start-up phase (SatDump does so once it has started).
            completeLoggerInit();
            logger->add_sink(sink);
        }
        ~LogCapture() { logger->del_sink(sink); }

        // How many lines hold both texts
        int count(const std::string &a, const std::string &b)
        {
            std::lock_guard<std::mutex> lg(sink->mtx);
            int n = 0;
            for (auto &l : sink->lines)
                if (l.find(a) != std::string::npos && l.find(b) != std::string::npos)
                    n++;
            return n;
        }

        // True if one line holds both texts
        bool has(const std::string &a, const std::string &b) { return count(a, b) > 0; }
    };

    // Polls until cond() is true or the time is up
    bool within(double seconds, const std::function<bool()> &cond)
    {
        auto end = Clock::now() + std::chrono::duration<double>(seconds);
        while (Clock::now() < end)
        {
            if (cond())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return cond();
    }

    // Runs fn and returns how long it took, in seconds. If it has not returned after
    // `deadline` seconds the test run is ended, since a hung DSP thread cannot be recovered.
    double timed(const std::function<void()> &fn, double deadline = 10)
    {
        std::atomic<bool> done{false};
        auto start = Clock::now();
        std::thread th([&]
                       {
                           fn();
                           done = true;
                       });
        if (!within(deadline, [&] { return done.load(); }))
        {
            std::fprintf(stderr, "test_block_lifetime: a stop or run did not return within %.0f s, giving up\n", deadline);
            std::fflush(stderr);
            std::_Exit(1);
        }
        th.join();
        return std::chrono::duration<double>(Clock::now() - start).count();
    }

    // New DSP: sends buffers of 64 floats until stop(true), or until `limit` were sent,
    // then a terminator. work() throws instead of sending when `fail` is set.
    class TestSource : public Block
    {
    public:
        std::atomic<int> sent{0};
        std::atomic<int> allocating{0}; // Buffers asked for, the last one maybe still waited for
        int limit = -1;
        bool fail = false;
        bool fail_unknown = false;

        TestSource() : Block("test_source", {}, {{"out", DSP_SAMPLE_TYPE_F32}}) {}
        nlohmann::json get_cfg(std::string) { return nullptr; }
        cfg_res_t set_cfg(std::string, nlohmann::json) { return RES_OK; }

    private:
        bool work()
        {
            if (fail_unknown)
                throw 42;
            if (fail)
                throw satdump_exception("test source fault");
            if (work_should_exit || (limit >= 0 && sent >= limit))
            {
                outputs[0].fifo->wait_enqueue(outputs[0].fifo->newBufferTerminator());
                return true;
            }
            allocating++;
            DSPBuffer b = outputs[0].fifo->newBufferSamples(64, sizeof(float));
            for (int i = 0; i < 64; i++)
                b.getSamples<float>()[i] = float(i);
            b.size = 64;
            outputs[0].fifo->wait_enqueue(b);
            sent++;
            return false;
        }
    };

    // New DSP: passes buffers on. work() throws once `throw_after` buffers went through.
    class TestPass : public Block
    {
    public:
        std::atomic<int> passed{0};
        int throw_after = -1;

        TestPass(std::string id) : Block(id, {{"in", DSP_SAMPLE_TYPE_F32}}, {{"out", DSP_SAMPLE_TYPE_F32}}) {}
        nlohmann::json get_cfg(std::string) { return nullptr; }
        cfg_res_t set_cfg(std::string, nlohmann::json) { return RES_OK; }

    private:
        bool work()
        {
            DSPBuffer i = inputs[0].fifo->wait_dequeue();
            if (i.isTerminator())
            {
                inputs[0].fifo->free(i);
                outputs[0].fifo->wait_enqueue(outputs[0].fifo->newBufferTerminator());
                return true;
            }
            if (throw_after >= 0 && passed >= throw_after)
            {
                inputs[0].fifo->free(i);
                throw satdump_exception("test fault in work()");
            }
            DSPBuffer o = outputs[0].fifo->newBufferSamples(i.size, sizeof(float));
            std::memcpy(o.getSamples<float>(), i.getSamples<float>(), i.size * sizeof(float));
            o.size = i.size;
            inputs[0].fifo->free(i);
            outputs[0].fifo->wait_enqueue(o);
            passed++;
            return false;
        }
    };

    // New DSP: takes buffers until a terminator. `total` adds up the samples; read it
    // after stop(), which joins the block's thread.
    class TestSink : public Block
    {
    public:
        std::atomic<int> received{0};
        std::atomic<bool> ended{false};
        double total = 0;

        TestSink() : Block("test_sink", {{"in", DSP_SAMPLE_TYPE_F32}}, {}) {}
        nlohmann::json get_cfg(std::string) { return nullptr; }
        cfg_res_t set_cfg(std::string, nlohmann::json) { return RES_OK; }

    private:
        bool work()
        {
            DSPBuffer i = inputs[0].fifo->wait_dequeue();
            bool terminator = i.isTerminator();
            if (!terminator)
                for (uint32_t k = 0; k < i.size; k++)
                    total += i.getSamples<float>()[k];
            inputs[0].fifo->free(i);
            if (terminator)
            {
                ended = true;
                return true;
            }
            received++;
            return false;
        }
    };

    // New DSP: two settings; "rate" must be a number
    class TestCfg : public Block
    {
    public:
        int rate = 0;
        std::string name;

        TestCfg() : Block("test_cfg") {}
        nlohmann::json get_cfg(std::string) { return nullptr; }
        cfg_res_t set_cfg(std::string key, nlohmann::json v)
        {
            if (key == "rate")
                rate = v; // Throws when v is not a number
            else if (key == "name")
                name = v;
            return RES_OK;
        }

    private:
        bool work() { return true; }
    };

    // New DSP: its start() fails (init() throws, as on a bad setting or a missing device).
    // `before_fail`, if set, runs first (a slow device, while the blocks already started run).
    class TestBadStart : public TestPass
    {
    public:
        std::function<void()> before_fail;
        TestBadStart() : TestPass("test_bad_start") {}

    protected:
        void init()
        {
            if (before_fail)
                before_fail();
            throw satdump_exception("test block cannot start");
        }
    };

    // New DSP: its start() throws something that is not a std::exception
    class TestBadStartUnknown : public TestPass
    {
    public:
        TestBadStartUnknown() : TestPass("test_bad_start_unknown") {}

    protected:
        void init() { throw 42; }
    };

    // New DSP: a source (sends until stop(true)) whose stop() throws once the block stopped
    class TestStopFaultSource : public TestSource
    {
    public:
        void stop(bool stop_now = false, bool force = false)
        {
            TestSource::stop(stop_now, force);
            throw satdump_exception("test_stop_fault cannot stop cleanly");
        }
    };

    // Legacy DSP: copies its input to its output. work() throws on buffer `fail_at` (the
    // third by default) when `fail` is set.
    class LegacyPass : public dsp::Block<float, float>
    {
    public:
        bool fail = false;
        int fail_at = 3;
        std::atomic<int> entered{0}; // work() calls begun

        LegacyPass(std::shared_ptr<dsp::stream<float>> in) : dsp::Block<float, float>(in) {}

        // Ends the block's thread the way some owners do, without join()
        void end_thread_without_join()
        {
            should_run = false;
            input_stream->stopReader();
        }

    private:
        int seen = 0;

        void work()
        {
            entered++;
            int n = input_stream->read();
            if (n <= 0)
            {
                input_stream->flush();
                return;
            }
            if (fail && ++seen >= fail_at)
                throw satdump_exception("legacy test fault");
            std::memcpy(output_stream->writeBuf, input_stream->readBuf, n * sizeof(float));
            input_stream->flush();
            output_stream->swap(n);
        }
    };
} // namespace

TEST_CASE("block lifetime: new DSP block whose work() throws is logged and its neighbours end")
{
    LogCapture log;
    {
        auto src = std::make_shared<TestSource>();
        auto mid = std::make_shared<TestPass>("test_fail");
        auto snk = std::make_shared<TestSink>();
        mid->throw_after = 3;
        mid->link(src.get(), 0, 0, 4);
        snk->link(mid.get(), 0, 0, 4);
        snk->start();
        mid->start();
        src->start();

        // The block after it gets the end of stream
        CHECK(within(2, [&] { return snk->ended.load(); }));
        CHECK(snk->received == 3);
        CHECK(log.has("test_fail", "test fault in work()"));
        CHECK(mid->has_failed());
        CHECK(mid->get_failure().find("test fault in work()") != std::string::npos);
        CHECK_FALSE(snk->has_failed());

        // The block before it is not left waiting for room: its buffers are still taken
        int sent = src->sent;
        CHECK(within(2, [&] { return src->sent > sent + 20; }));

        // Each stop is quick. The source sends its terminator, which ends the drained input.
        CHECK(timed([&] { src->stop(true); }) < 2);
        CHECK(timed([&] { mid->stop(); }) < 2);
        CHECK(timed([&] { snk->stop(); }) < 2);
    } // Destroying them (all stopped) is fine
}

TEST_CASE("block lifetime: new DSP source whose work() throws ends the blocks after it")
{
    LogCapture log;
    auto src = std::make_shared<TestSource>();
    auto snk = std::make_shared<TestSink>();
    src->fail_unknown = true; // Not a std::exception
    snk->link(src.get(), 0, 0, 4);
    snk->start();
    src->start();

    CHECK(within(2, [&] { return snk->ended.load(); }));
    CHECK(log.has("test_source", "unknown exception"));
    CHECK(src->has_failed());
    CHECK(timed([&] { src->stop(); }) < 2);
    CHECK(timed([&] { snk->stop(); }) < 2);

    // A new start() begins without the old failure
    src->fail_unknown = false;
    src->limit = 5;
    auto snk2 = std::make_shared<TestSink>();
    snk2->link(src.get(), 0, 0, 4);
    snk2->start();
    src->start();
    CHECK(within(2, [&] { return snk2->ended.load(); }));
    CHECK(snk2->received == 5);
    CHECK(timed([&] { src->stop(); }) < 2);
    CHECK(timed([&] { snk2->stop(); }) < 2);
    CHECK_FALSE(src->has_failed());
    CHECK(src->get_failure().empty());
}

TEST_CASE("block lifetime: new DSP failed block whose reader stopped does not hang stop()")
{
    LogCapture log;
    auto src = std::make_shared<TestSource>();
    auto mid = std::make_shared<TestPass>("test_fail");
    auto snk = std::make_shared<TestSink>(); // Never started: nothing takes mid's output
    mid->throw_after = 4;                    // Fills the 4-buffer output first, no room left
    mid->link(src.get(), 0, 0, 4);
    snk->link(mid.get(), 0, 0, 4);
    mid->start();
    src->start();

    CHECK(within(2, [&] { return mid->has_failed(); }));
    CHECK(timed([&] { src->stop(true); }) < 2);
    // Gives up on the output 2 s after its input ended
    CHECK(timed([&] { mid->stop(); }) < 4);
    CHECK(log.has("test_fail", "end of stream was not passed on"));
}

TEST_CASE("block lifetime: new DSP blocks started and stopped normally are destroyed normally")
{
    LogCapture log;
    {
        TestSource never_started;
        auto src = std::make_shared<TestSource>();
        auto mid = std::make_shared<TestPass>("test_pass");
        auto snk = std::make_shared<TestSink>();
        src->limit = 50;
        mid->link(src.get(), 0, 0, 4);
        snk->link(mid.get(), 0, 0, 4);
        snk->start();
        mid->start();
        src->start();
        CHECK(within(2, [&] { return snk->ended.load(); }));
        CHECK(snk->received == 50);
        CHECK(timed([&] { src->stop(); }) < 2);
        CHECK(timed([&] { mid->stop(); }) < 2);
        CHECK(timed([&] { snk->stop(); }) < 2);
        CHECK_FALSE(mid->has_failed());
    }
    CHECK_FALSE(log.has("destroyed while running", ""));
}

TEST_CASE("block lifetime: new DSP setting that cannot be applied is logged")
{
    LogCapture log;
    TestCfg cfg;
    Block &blk = cfg; // The JSON form of set_cfg() is the base class's
    CHECK(blk.set_cfg(nlohmann::json{{"rate", "fast"}, {"name", "kept"}}) == Block::RES_ERR);
    CHECK(log.has("test_cfg", "setting rate was not applied"));
    CHECK(cfg.name == "kept"); // The other settings are still applied
}

TEST_CASE("block lifetime: new DSP blocks that ended with their stream are destroyed without stop()")
{
    // Their threads have finished, so ~Block joins them. This used to throw from the
    // destructor, which is std::terminate.
    LogCapture log;
    {
        auto src = std::make_shared<TestSource>();
        auto snk = std::make_shared<TestSink>();
        src->limit = 10;
        snk->link(src.get(), 0, 0, 4);
        snk->start();
        src->start();

        bool ended = within(4, [&] { return src->has_ended() && snk->has_ended(); });
        CHECK(ended);
        CHECK(snk->received == 10);
        if (!ended) // Not the case being tested: stop them so the test run goes on
        {
            src->stop(true, true);
            snk->stop(true, true);
        }
    } // Destroyed without stop()
    CHECK_FALSE(log.has("destroyed while running", ""));
}

TEST_CASE("block lifetime: new DSP block that failed is destroyed without stop()")
{
    LogCapture log;
    auto src = std::make_shared<TestSource>();
    auto snk = std::make_shared<TestSink>();
    src->limit = 10;
    {
        auto mid = std::make_shared<TestPass>("test_fail");
        mid->throw_after = 3;
        mid->link(src.get(), 0, 0, 4);
        snk->link(mid.get(), 0, 0, 4);
        snk->start();
        mid->start();
        src->start();

        // It has ended once the source's end of stream reached it
        bool ended = within(4, [&] { return mid->has_ended(); });
        CHECK(ended);
        CHECK(mid->has_failed());
        if (!ended) // Not the case being tested: stop it so the test run goes on
            mid->stop(true, true);
    } // Destroyed without stop()
    CHECK_FALSE(log.has("destroyed while running", ""));
    CHECK(timed([&] { src->stop(); }) < 2);
    CHECK(timed([&] { snk->stop(); }) < 2);
}

TEST_CASE("block lifetime: new DSP start failure stops the blocks already started")
{
    LogCapture log;
    // src -> snk, and src2 -> bad -> snk2. src sends until it is stopped.
    auto src = std::make_shared<TestSource>();
    auto snk = std::make_shared<TestSink>();
    auto src2 = std::make_shared<TestSource>();
    auto bad = std::make_shared<TestBadStart>();
    auto snk2 = std::make_shared<TestSink>();
    snk->link(src.get(), 0, 0, 4);
    bad->link(src2.get(), 0, 0, 4);
    snk2->link(bad.get(), 0, 0, 4);

    bool threw = false;
    double took = timed([&]
                        {
                            try
                            {
                                // snk, src and snk2 start (src begins sending), then bad fails
                                start_blocks_or_undo({snk.get(), src.get(), snk2.get(), bad.get(), src2.get()});
                            }
                            catch (const std::exception &e)
                            {
                                threw = std::string(e.what()).find("cannot start") != std::string::npos;
                            }
                        });
    CHECK(threw);
    CHECK(took < 2);
    // The started ones ended through their terminator (a stopped source sends one)
    CHECK(snk->ended);
    CHECK(snk2->ended);
    CHECK(src->has_ended()); // It was started, and stopped again
    // Destroying them now is fine: they were stopped
}

TEST_CASE("block lifetime: new DSP Add after a branch that failed lets the source run on and stop")
{
    // src -> splitter -> {test_fail, test_pass} -> Add -> snk. The Add ended on the failed
    // branch's end of stream and left the healthy branch's input full, so the splitter and then
    // the source waited for room forever, and stopping the source hung.
    LogCapture log;
    auto src = std::make_shared<TestSource>();
    auto spl = std::make_shared<SplitterBlock<float>>();
    auto bad = std::make_shared<TestPass>("test_fail");
    auto good = std::make_shared<TestPass>("test_pass");
    auto add = std::make_shared<AddBlock<float>>();
    auto snk = std::make_shared<TestSink>();
    spl->set_cfg("noutputs", 2);
    bad->throw_after = 3;
    spl->link(src.get(), 0, 0, 4);
    bad->link(spl.get(), 0, 0, 4);
    good->link(spl.get(), 1, 0, 4);
    add->link(bad.get(), 0, 0, 4);
    add->link(good.get(), 0, 1, 4);
    snk->link(add.get(), 0, 0, 4);
    start_blocks_or_undo({snk.get(), add.get(), bad.get(), good.get(), spl.get(), src.get()});

    CHECK(within(2, [&] { return snk->ended.load(); }));
    CHECK(snk->received == 3);
    CHECK(log.has("test_fail", "test fault in work()"));

    // The source is not left waiting for room: both branches are still taken
    int sent = src->sent;
    CHECK(within(2, [&] { return src->sent > sent + 20; }));

    CHECK(timed([&] { src->stop(true); }) < 2);
    CHECK(timed([&] { spl->stop(); }) < 2);
    CHECK(timed([&] { bad->stop(); }) < 2);
    CHECK(timed([&] { good->stop(); }) < 2);
    CHECK(timed([&] { add->stop(); }) < 2);
    CHECK(timed([&] { snk->stop(); }) < 2);
}

TEST_CASE("block lifetime: new DSP start failure with an Add fed by a started source stops it")
{
    // src -> Add.in0 and bad -> Add.in1, Add -> snk. snk, Add and src start, then bad fails to
    // start once src has filled the Add's input and waits for room. The Add ended on the end
    // of stream sent to its unused input and kept one of src's buffers, so src waited for room
    // forever and the undo hung.
    LogCapture log;
    auto src = std::make_shared<TestSource>();
    auto bad = std::make_shared<TestBadStart>();
    auto add = std::make_shared<AddBlock<float>>();
    auto snk = std::make_shared<TestSink>();
    add->link(src.get(), 0, 0, 4);
    add->link(bad.get(), 0, 1, 4);
    snk->link(add.get(), 0, 0, 4);
    // The Add holds one buffer and waits for its other input, 3 are queued: the 5th waits
    bool src_waits = false;
    bad->before_fail = [&] { src_waits = within(2, [&] { return src->allocating >= 5; }); };

    bool threw = false;
    double took = timed([&]
                        {
                            try
                            {
                                start_blocks_or_undo({snk.get(), add.get(), src.get(), bad.get()});
                            }
                            catch (const std::exception &e)
                            {
                                threw = std::string(e.what()).find("cannot start") != std::string::npos;
                            }
                        });
    CHECK(src_waits);
    CHECK(threw);
    CHECK(took < 4); // Up to 2 s of it is the wait for src above
    CHECK(snk->ended);
}

TEST_CASE("block lifetime: new DSP Add whose inputs end at different times lets the longer one finish")
{
    // The normal end of a stream through a block with two inputs: the short input ends the
    // Add (sums unchanged), and the long one is still taken to its end, so its source finishes
    // instead of waiting for room forever.
    LogCapture log;
    auto shrt = std::make_shared<TestSource>();
    auto lng = std::make_shared<TestSource>();
    auto add = std::make_shared<AddBlock<float>>();
    auto snk = std::make_shared<TestSink>();
    shrt->limit = 5;
    lng->limit = 200;
    add->link(shrt.get(), 0, 0, 4);
    add->link(lng.get(), 0, 1, 4);
    snk->link(add.get(), 0, 0, 4);
    start_blocks_or_undo({snk.get(), add.get(), shrt.get(), lng.get()});

    CHECK(within(2, [&] { return snk->ended.load(); }));
    CHECK(within(2, [&] { return lng->sent == 200; }));
    CHECK(timed([&] { shrt->stop(); }) < 2);
    CHECK(timed([&] { lng->stop(); }) < 2);
    CHECK(timed([&] { add->stop(); }) < 2);
    CHECK(timed([&] { snk->stop(); }) < 2);
    CHECK(snk->received == 5);
    CHECK(snk->total == doctest::Approx(5 * 2 * 2016.0)); // 2 x (0 + 1 + ... + 63) per buffer
    CHECK_FALSE(log.has("failed", ""));
}

TEST_CASE("block lifetime: new DSP selector that ended on one input lets the other inputs finish")
{
    // The selector ends on whichever input ends first. The short source runs alone first, so
    // that is input 0; the long source starts after the selector ended and must still be able
    // to send everything (its input was left full, and its source waited for room forever).
    LogCapture log;
    auto shrt = std::make_shared<TestSource>();
    auto lng = std::make_shared<TestSource>();
    auto sel = std::make_shared<SelectorBlock<float>>();
    auto snk = std::make_shared<TestSink>();
    shrt->limit = 5;
    lng->limit = 200;
    sel->set_cfg("ninputs", 2);
    sel->link(shrt.get(), 0, 0, 4);
    sel->link(lng.get(), 0, 1, 4);
    snk->link(sel.get(), 0, 0, 4);
    start_blocks_or_undo({snk.get(), sel.get(), shrt.get()});

    CHECK(within(2, [&] { return snk->ended.load(); }));
    lng->start();
    CHECK(within(2, [&] { return lng->sent == 200; }));
    CHECK(timed([&] { shrt->stop(); }) < 2);
    CHECK(timed([&] { lng->stop(); }) < 2);
    CHECK(timed([&] { sel->stop(); }) < 2);
    CHECK(timed([&] { snk->stop(); }) < 2);
    CHECK(snk->received == 5); // Input 0 is the active one
}

#ifndef _WIN32
namespace
{
    // Writes each log line to a pipe (the test's child process uses it)
    struct FdSink : public slog::LoggerSink
    {
        int fd;
        FdSink(int fd) : fd(fd) {}
        void receive(slog::LogMsg log)
        {
            std::string s = log.str + "\n";
            size_t done = 0;
            while (done < s.size())
            {
                ssize_t w = write(fd, s.data() + done, s.size() - done);
                if (w <= 0)
                    return;
                done += size_t(w);
            }
        }
    };
} // namespace

TEST_CASE("block lifetime: new DSP block destroyed while running stops with a log line and not std::terminate")
{
    // ~Block cannot stop a running block safely (see block.h), so this is a deliberate abort.
    // It runs in a child process; std::terminate there would exit with 42 instead. The source
    // is first left waiting for room in its output, where running blocks spend their time. (A
    // block destroyed between two work() calls can stop first with "pure virtual method
    // called", as its derived part is gone by the time ~Block runs.)
    int fds[2];
    REQUIRE(pipe(fds) == 0);
    std::fflush(nullptr);
    pid_t pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0)
    {
        close(fds[0]);
        std::signal(SIGABRT, SIG_DFL); // Not the test runner's crash report, this abort is expected
        std::set_terminate([] { std::_Exit(42); });
        completeLoggerInit(); // Only this process's own lines from here on (see LogCapture)
        logger->add_sink(std::make_shared<FdSink>(fds[1]));
        TestSource *src = new TestSource();
        src->get_output(0, 4);
        src->start();
        // 4 buffers fill the output; the 5th waits for one to come back, which never happens
        if (!within(5, [&] { return src->allocating >= 5; }))
            std::_Exit(3);
        delete src; // Still running
        std::_Exit(0);
    }
    close(fds[1]);
    std::string text;
    char buf[512];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof(buf))) > 0)
        text.append(buf, size_t(n));
    close(fds[0]);
    int status = 0;
    REQUIRE(waitpid(pid, &status, 0) == pid);

    CHECK(WIFSIGNALED(status));
    if (WIFSIGNALED(status))
        CHECK(WTERMSIG(status) == SIGABRT);
    if (WIFEXITED(status))
        CHECK_MESSAGE(WEXITSTATUS(status) != 42, "std::terminate was called");
    CHECK(text.find("test_source") != std::string::npos);
    CHECK(text.find("destroyed while running") != std::string::npos);
}
#endif

TEST_CASE("block lifetime: flowgraph with a block whose work() fails finishes and reports it")
{
    LogCapture log;
    flowgraph::Flowgraph fg;
    fg.node_internal_registry["t_src"] = {"Test/Source", [](const flowgraph::Flowgraph *f)
                                          {
                                              auto b = std::make_shared<TestSource>();
                                              b->limit = 200;
                                              return std::make_shared<flowgraph::NodeInternal>(f, b);
                                          }};
    fg.node_internal_registry["t_fail"] = {"Test/Fail", [](const flowgraph::Flowgraph *f)
                                           {
                                               auto b = std::make_shared<TestPass>("test_fail");
                                               b->throw_after = 3;
                                               return std::make_shared<flowgraph::NodeInternal>(f, b);
                                           }};
    fg.node_internal_registry["t_sink"] = {"Test/Sink", [](const flowgraph::Flowgraph *f)
                                           { return std::make_shared<flowgraph::NodeInternal>(f, std::make_shared<TestSink>()); }};

    auto io = [](int id, const char *name, bool out) { return nlohmann::json{{"id", id}, {"name", name}, {"is_out", out}}; };
    nlohmann::json j;
    j["nodes"] = {{{"id", 0}, {"int_id", "t_src"}, {"io", {io(1, "out", true)}}},
                  {{"id", 1}, {"int_id", "t_fail"}, {"io", {io(2, "in", false), io(3, "out", true)}}},
                  {{"id", 2}, {"int_id", "t_sink"}, {"io", {io(4, "in", false)}}}};
    j["links"] = {{{"id", 0}, {"start", 1}, {"end", 2}}, {{"id", 1}, {"start", 3}, {"end", 4}}};
    fg.setJSON(j);

    bool ok = true;
    CHECK(timed([&] { ok = fg.run(); }) < 5);
    CHECK_FALSE(ok);
    CHECK(log.has("test_fail", "test fault in work()"));
    CHECK(log.has("Flowgraph block test_fail failed", "test fault in work()"));
}

TEST_CASE("block lifetime: flowgraph start failure stops the blocks already started")
{
    LogCapture log;
    flowgraph::Flowgraph fg;
    fg.node_internal_registry["t_src"] = {"Test/Source", [](const flowgraph::Flowgraph *f)
                                          { return std::make_shared<flowgraph::NodeInternal>(f, std::make_shared<TestSource>()); }};
    fg.node_internal_registry["t_bad"] = {"Test/Bad", [](const flowgraph::Flowgraph *f)
                                          { return std::make_shared<flowgraph::NodeInternal>(f, std::make_shared<TestBadStart>()); }};
    fg.node_internal_registry["t_sink"] = {"Test/Sink", [](const flowgraph::Flowgraph *f)
                                           { return std::make_shared<flowgraph::NodeInternal>(f, std::make_shared<TestSink>()); }};

    // src1 -> sink1 runs (src1 sends until stopped) before bad fails to start; src2 -> bad -> sink2
    auto io = [](int id, const char *name, bool out) { return nlohmann::json{{"id", id}, {"name", name}, {"is_out", out}}; };
    nlohmann::json j;
    j["nodes"] = {{{"id", 0}, {"int_id", "t_sink"}, {"io", {io(1, "in", false)}}},
                  {{"id", 1}, {"int_id", "t_src"}, {"io", {io(2, "out", true)}}},
                  {{"id", 2}, {"int_id", "t_sink"}, {"io", {io(3, "in", false)}}},
                  {{"id", 3}, {"int_id", "t_bad"}, {"io", {io(4, "in", false), io(5, "out", true)}}},
                  {{"id", 4}, {"int_id", "t_src"}, {"io", {io(6, "out", true)}}}};
    j["links"] = {{{"id", 0}, {"start", 2}, {"end", 1}}, {{"id", 1}, {"start", 5}, {"end", 3}}, {{"id", 2}, {"start", 6}, {"end", 4}}};
    fg.setJSON(j);

    bool ok = true;
    CHECK(timed([&] { ok = fg.run(); }) < 2);
    CHECK_FALSE(ok);
    CHECK(log.has("Error running flowgraph", "cannot start"));
    CHECK_FALSE(fg.isRunning());
    // Destroying the flowgraph destroys its blocks: none may still be running
}

TEST_CASE("block lifetime: flowgraph whose block start() throws something else than std::exception reports it")
{
    LogCapture log;
    flowgraph::Flowgraph fg;
    fg.node_internal_registry["t_src"] = {"Test/Source", [](const flowgraph::Flowgraph *f)
                                          { return std::make_shared<flowgraph::NodeInternal>(f, std::make_shared<TestSource>()); }};
    fg.node_internal_registry["t_bad"] = {"Test/Bad", [](const flowgraph::Flowgraph *f)
                                          { return std::make_shared<flowgraph::NodeInternal>(f, std::make_shared<TestBadStartUnknown>()); }};
    fg.node_internal_registry["t_sink"] = {"Test/Sink", [](const flowgraph::Flowgraph *f)
                                           { return std::make_shared<flowgraph::NodeInternal>(f, std::make_shared<TestSink>()); }};

    // src -> bad -> sink: sink starts, then bad throws 42 (it was not caught: std::terminate)
    auto io = [](int id, const char *name, bool out) { return nlohmann::json{{"id", id}, {"name", name}, {"is_out", out}}; };
    nlohmann::json j;
    j["nodes"] = {{{"id", 0}, {"int_id", "t_src"}, {"io", {io(1, "out", true)}}},
                  {{"id", 1}, {"int_id", "t_bad"}, {"io", {io(2, "in", false), io(3, "out", true)}}},
                  {{"id", 2}, {"int_id", "t_sink"}, {"io", {io(4, "in", false)}}}};
    j["links"] = {{{"id", 0}, {"start", 1}, {"end", 2}}, {{"id", 1}, {"start", 3}, {"end", 4}}};
    fg.setJSON(j);

    bool ok = true;
    CHECK(timed([&] { ok = fg.run(); }) < 2);
    CHECK_FALSE(ok);
    CHECK(log.has("Error running flowgraph", "unknown exception"));
    CHECK_FALSE(fg.isRunning());
}

TEST_CASE("block lifetime: flowgraph source whose stop() throws is logged and the flowgraph still ends")
{
    LogCapture log;
    flowgraph::Flowgraph fg;
    fg.node_internal_registry["t_src"] = {"Test/Source", [](const flowgraph::Flowgraph *f)
                                          { return std::make_shared<flowgraph::NodeInternal>(f, std::make_shared<TestStopFaultSource>()); }};
    fg.node_internal_registry["t_sink"] = {"Test/Sink", [](const flowgraph::Flowgraph *f)
                                           { return std::make_shared<flowgraph::NodeInternal>(f, std::make_shared<TestSink>()); }};
    auto io = [](int id, const char *name, bool out) { return nlohmann::json{{"id", id}, {"name", name}, {"is_out", out}}; };
    nlohmann::json j;
    j["nodes"] = {{{"id", 0}, {"int_id", "t_src"}, {"io", {io(1, "out", true)}}},
                  {{"id", 1}, {"int_id", "t_sink"}, {"io", {io(2, "in", false)}}}};
    j["links"] = {{{"id", 0}, {"start", 1}, {"end", 2}}};
    fg.setJSON(j);

    // The source sends until it is stopped, as the UI's Stop button does with Flowgraph::stop()
    std::atomic<bool> ran{false};
    bool ok = true;
    std::thread runner([&]
                       {
                           ok = fg.run();
                           ran = true;
                       });
    CHECK(within(2, [&] { return fg.isRunning(); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(timed([&] { fg.stop(); }) < 2); // Its stop thread threw: std::terminate
    bool ended = within(4, [&] { return ran.load(); });
    CHECK(ended);
    if (!ended) // Never hang the test run
    {
        std::fprintf(stderr, "test_block_lifetime: the flowgraph did not end, giving up\n");
        std::fflush(stderr);
        std::_Exit(1);
    }
    runner.join();
    CHECK(log.has("Error stopping flowgraph block test_source", "test_stop_fault"));
    CHECK_FALSE(ok); // run() stops it once more and reports that too
}

TEST_CASE("block lifetime: legacy block whose work() throws is logged and the chain winds down")
{
    LogCapture log;
    auto in = std::make_shared<dsp::stream<float>>(1024);
    auto a = std::make_shared<LegacyPass>(in);
    auto b = std::make_shared<LegacyPass>(a->output_stream);
    a->fail = true;
    a->start();
    b->start();

    // The writer before the chain, like a demodulator's file reader: 50 buffers
    std::atomic<int> handed{0};
    std::atomic<bool> writer_done{false};
    std::thread writer([&]
                       {
                           for (int i = 0; i < 50; i++)
                           {
                               for (int k = 0; k < 64; k++)
                                   in->writeBuf[k] = float(i);
                               if (!in->swap(64))
                                   break;
                               handed++;
                           }
                           writer_done = true;
                       });

    // The reader after the chain, like a demodulator's loop: reads until the writer is done
    std::atomic<bool> reader_done{false};
    std::thread reader([&]
                       {
                           while (!writer_done)
                           {
                               b->output_stream->read();
                               b->output_stream->flush();
                           }
                           reader_done = true;
                       });

    bool writer_ok = within(3, [&] { return writer_done.load(); });
    bool reader_ok = within(3, [&] { return reader_done.load(); });
    CHECK(writer_ok);
    CHECK(reader_ok);
    if (!writer_ok) // Never hang the test run
        in->stopWriter();
    if (!reader_ok)
    {
        writer_done = true;
        b->output_stream->stopReader();
    }
    writer.join();
    reader.join();

    CHECK(handed == 50); // The failed block kept taking its input
    CHECK(log.has("LegacyPass", "legacy test fault"));
    CHECK(a->has_failed());
    CHECK(a->failure_message().find("legacy test fault") != std::string::npos);
    CHECK_FALSE(b->has_failed());
    CHECK(timed([&] { a->stop(); }) < 2);
    CHECK(timed([&] { b->stop(); }) < 2);

    // Started again, it begins without the old failure (both the flag and the message)
    in->clearReadStop();
    a->output_stream->clearWriteStop();
    a->fail = false;
    a->start();
    CHECK_FALSE(a->has_failed());
    CHECK(a->failure_message().empty());
    CHECK(timed([&] { a->stop(); }) < 2);
}

TEST_CASE("block lifetime: legacy block whose thread ended without join is joined by its destructor")
{
    // Each block is first left waiting for input inside work(), where running blocks spend
    // their time. (One destroyed between two work() calls can stop with "pure virtual method
    // called", as its derived part is gone by the time ~Block runs; see block.h.)
    LogCapture log;
    auto in = std::make_shared<dsp::stream<float>>(1024);
    {
        LegacyPass a(in);
        a.start();
        REQUIRE(within(2, [&] { return a.entered >= 1; }));
        a.end_thread_without_join();
    } // A joinable std::thread destroyed here was std::terminate
    CHECK(log.count("BLOCK SHOULD BE STOPPED", "") == 1);

    {
        LegacyPass running(in);
        in->clearReadStop();
        running.start();
        REQUIRE(within(2, [&] { return running.entered >= 1; }));
    } // Destroyed while running: logged, stopped and joined (as upstream does)
    CHECK(log.count("BLOCK SHOULD BE STOPPED", "") == 2);
}

namespace
{
    // Commands for the command line test, registered once through the event bus as plugins do
    class TestCommand : public satdump::CmdHandler
    {
        std::function<void()> body;

    public:
        TestCommand(std::string name, std::function<void()> body) : satdump::CmdHandler(name), body(body) {}
        void reg(CLI::App *app) { app->add_subcommand(cmd, "test command"); }
        void run(CLI::App *, CLI::App *, bool) { body(); }
    };

    // A legacy chain like a demodulator's whose block fails on the first buffer
    void legacy_chain_fails()
    {
        auto in = std::make_shared<dsp::stream<float>>(1024);
        LegacyPass blk(in);
        blk.fail = true;
        blk.fail_at = 1;
        blk.start();
        for (int i = 0; i < 4 && !blk.has_failed(); i++)
        {
            for (int k = 0; k < 64; k++)
                in->writeBuf[k] = float(k);
            if (!in->swap(64))
                break;
        }
        within(2, [&] { return blk.has_failed(); });
        blk.stop();
    }

    // A new DSP chain whose middle block fails
    void ndsp_chain_fails()
    {
        auto src = std::make_shared<TestSource>();
        auto mid = std::make_shared<TestPass>("test_cli_fail");
        auto snk = std::make_shared<TestSink>();
        src->limit = 10;
        mid->throw_after = 1;
        mid->link(src.get(), 0, 0, 4);
        snk->link(mid.get(), 0, 0, 4);
        start_blocks_or_undo({snk.get(), mid.get(), src.get()});
        within(2, [&] { return snk->ended.load(); });
        src->stop();
        mid->stop();
        snk->stop();
    }

    // DSP blocks that run normally
    void ndsp_chain_works()
    {
        auto src = std::make_shared<TestSource>();
        auto snk = std::make_shared<TestSink>();
        src->limit = 10;
        snk->link(src.get(), 0, 0, 4);
        start_blocks_or_undo({snk.get(), src.get()});
        within(2, [&] { return snk->ended.load(); });
        src->stop();
        snk->stop();
    }

    void register_test_commands()
    {
        static bool done = false;
        if (done)
            return;
        done = true;
        if (!satdump::eventBus)
            satdump::eventBus = std::make_shared<satdump::EventBus>();
        satdump::eventBus->register_handler<satdump::cli::RegisterSubcommandEvent>(
            [](satdump::cli::RegisterSubcommandEvent evt)
            {
                evt.cmd_handlers.push_back(std::make_shared<TestCommand>("test_legacy_fail", legacy_chain_fails));
                evt.cmd_handlers.push_back(std::make_shared<TestCommand>("test_ndsp_fail", ndsp_chain_fails));
                evt.cmd_handlers.push_back(std::make_shared<TestCommand>("test_dsp_ok", ndsp_chain_works));
            });
    }

    // Runs `satdump <name>` the way main() does; true means the command failed (exit 2)
    bool command_failed(const std::string &name)
    {
        register_test_commands();
        satdump::cli::CommandHandler handler(false);
        std::string prog = "satdump", sub = name;
        char *argv[] = {&prog[0], &sub[0], nullptr};
        REQUIRE(handler.parse(2, argv) == 0);
        bool failed = true;
        timed([&] { failed = handler.run(); });
        return failed;
    }
} // namespace

TEST_CASE("block lifetime: a command during which a DSP block failed ends as failed")
{
    // The failed block is logged and contained, so the command finishes, but its output is
    // incomplete: the command line says so with exit 2. Before, this was std::terminate.
    LogCapture log;
    CHECK(command_failed("test_legacy_fail"));
    CHECK(log.has("Error running command", "LegacyPass"));
    CHECK(command_failed("test_ndsp_fail"));
    CHECK(log.has("Error running command", "test_cli_fail"));
}

TEST_CASE("block lifetime: a command during which DSP blocks ran normally succeeds")
{
    LogCapture log;
    CHECK_FALSE(command_failed("test_dsp_ok"));
    CHECK_FALSE(log.has("Error running command", ""));
}
