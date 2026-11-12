// Fork: a module or task that fails in its own thread must not end the program or stall the rest.
// Covers the offline runs, the live pipeline, the processing handlers, the task queue and the task
// scheduler.

#include "../../src-interface/main_ui.h" // SetIsDoneProcessingEvent, as processing.cpp includes it
#include "common/dsp/path/splitter.h"
#include "core/plugin.h"
#include "doctest.h"
#include "fault_module.h"
#include "handlers/processing/processing.h"
#include "handlers/processing_handler.h"
#include "libs/ctpl/ctpl_stl.h"
#include "pipeline/live_pipeline.h"
#include "pipeline/pipeline.h"
#include "utils/task_queue.h"
#include "utils/task_scheduler.h"
#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <thread>

namespace
{
    using namespace std::chrono_literals;
    using satdump::pipeline::LivePipeline;
    using satdump::pipeline::Pipeline;
    using satdump::pipeline::ProcessingModule;
    using Modules = std::vector<std::shared_ptr<ProcessingModule>>;

    void setup()
    {
        static bool done = false;
        if (done)
            return;
        done = true;
        if (!satdump::eventBus)
            satdump::eventBus = std::make_shared<satdump::EventBus>();
        fork_test::register_modules(satdump::pipeline::modules_registry);
    }

    std::string temp_dir(std::string name)
    {
        std::filesystem::path p = std::filesystem::temp_directory_path() /
                                  ("satdump_thread_faults_" + name + "_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(p);
        return p.string();
    }

    std::string write_input(std::string dir, size_t size)
    {
        std::string path = dir + "/input.bin";
        std::ofstream f(path, std::ios::binary);
        std::vector<char> data(size, 7);
        f.write(data.data(), data.size());
        return path;
    }

    // Baseband, then fork_fault_a, then fork_fault_b; both run live
    Pipeline make_pipeline(nlohmann::json params_a, nlohmann::json params_b)
    {
        Pipeline p;
        p.id = "fork_fault_test";
        p.name = "Fork fault test";
        p.live = true;
        p.live_cfg.normal_live = {1, 2};
        p.steps.push_back({"baseband", "", nlohmann::json::object(), ""});
        p.steps.push_back({"soft", "fork_fault_a", params_a, ""});
        p.steps.push_back({"frames", "fork_fault_b", params_b, ""});
        return p;
    }

    template <typename F>
    bool wait_for(F cond, std::chrono::milliseconds limit)
    {
        auto start = std::chrono::steady_clock::now();
        while (!cond())
        {
            if (std::chrono::steady_clock::now() - start > limit)
                return false;
            std::this_thread::sleep_for(5ms);
        }
        return true;
    }

    // A splitter fed with samples, with a "record" output that is counted (as a recording would be)
    // and a "live" output for the live pipeline
    struct LiveRig
    {
        std::shared_ptr<dsp::stream<complex_t>> src = std::make_shared<dsp::stream<complex_t>>();
        dsp::SplitterBlock splitter{src};
        std::atomic<bool> feeding{true};
        std::atomic<uint64_t> recorded{0};
        std::thread feeder, recorder;

        LiveRig()
        {
            splitter.set_main_enabled(false);
            splitter.add_output("record");
            splitter.set_enabled("record", true);
            splitter.add_output("live");
            splitter.set_enabled("live", true);
            splitter.start();
            feeder = std::thread(
                [this]()
                {
                    while (feeding && src->swap(4096))
                        std::this_thread::sleep_for(1ms);
                });
            std::shared_ptr<dsp::stream<complex_t>> rec = splitter.get_output("record");
            recorder = std::thread(
                [this, rec]()
                {
                    int n;
                    while ((n = rec->read()) > 0)
                    {
                        recorded += n;
                        rec->flush();
                    }
                });
        }

        std::shared_ptr<dsp::stream<complex_t>> live() { return splitter.get_output("live"); }

        ~LiveRig()
        {
            feeding = false;
            src->stopWriter();
            feeder.join();
            splitter.get_output("record")->stopWriter();
            splitter.get_output("live")->stopWriter();
            splitter.stop();
            splitter.get_output("record")->stopReader();
            recorder.join();
        }
    };

    // Stops every buffer by hand, so modules left running by a failed stop() still end and the
    // test can go on
    void rescue(Modules &mods)
    {
        for (auto &m : mods)
        {
            m->input_active = false;
            if (m->input_fifo)
            {
                m->input_fifo->stopReader();
                m->input_fifo->stopWriter();
            }
            if (m->output_fifo)
            {
                m->output_fifo->stopReader();
                m->output_fifo->stopWriter();
            }
            if (m->input_stream)
            {
                m->input_stream->stopReader();
                m->input_stream->stopWriter();
            }
        }
    }

    struct StopResult
    {
        bool in_time = false;
        bool threw = false;
        double seconds = 0;
    };

    // Calls stop() in a helper thread; if it has not returned after the limit, the buffers are
    // stopped by hand so it can
    StopResult stop_live(LivePipeline &lp, Modules mods, std::chrono::milliseconds limit)
    {
        StopResult r;
        std::atomic<bool> done{false}, threw{false};
        auto start = std::chrono::steady_clock::now();
        std::thread t(
            [&]()
            {
                try
                {
                    lp.stop();
                }
                catch (...)
                {
                    threw = true;
                }
                done = true;
            });
        r.in_time = wait_for([&]() { return (bool)done; }, limit);
        r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        rescue(mods);
        t.join();
        rescue(mods);
        r.threw = threw;
        return r;
    }

    uint64_t bytes_in(std::shared_ptr<ProcessingModule> mod) { return ((fork_test::FaultModule *)mod.get())->bytes_in; }
} // namespace

TEST_CASE("Two-module run: module 1 failing in its thread is reported to the caller, and module 2 is stopped")
{
    setup();
    std::string dir = temp_dir("m1");
    std::string input = write_input(dir, 256 * 1024);
    Pipeline p = make_pipeline({{"fail_after", 2}}, nlohmann::json::object());

    // The program used to end here (std::terminate)
    std::string error;
    try
    {
        p.run(input, dir, nlohmann::json::object(), "baseband");
    }
    catch (const std::exception &e)
    {
        error = e.what();
    }
    CHECK(error.find("failure on purpose") != std::string::npos);
    CHECK(fork_test::running_modules() == 0);
    std::filesystem::remove_all(dir);
}

TEST_CASE("Two-module run: module 2 failing does not leave module 1 waiting on a full buffer")
{
    setup();
    std::string dir = temp_dir("m2");
    // Module 1 writes 4 MB, more than the 1 MB buffer holds once module 2 is gone
    std::string input = write_input(dir, 512 * 1024);
    Pipeline p = make_pipeline({{"out_factor", 8}}, {{"fail_after", 2}});

    auto start = std::chrono::steady_clock::now();
    std::string error;
    try
    {
        p.run(input, dir, nlohmann::json::object(), "baseband");
    }
    catch (const std::exception &e)
    {
        error = e.what();
    }
    CHECK(error.find("failure on purpose") != std::string::npos);
    CHECK(std::chrono::steady_clock::now() - start < 20s);
    CHECK(fork_test::running_modules() == 0);
    std::filesystem::remove_all(dir);
}

TEST_CASE("Two-module run: an error that is not a std::exception is reported too")
{
    setup();
    std::string dir = temp_dir("other");
    std::string input = write_input(dir, 64 * 1024);
    Pipeline p = make_pipeline(nlohmann::json::object(), {{"fail_after", 1}, {"fail_with", "other"}});

    std::string error;
    try
    {
        p.run(input, dir, nlohmann::json::object(), "baseband");
    }
    catch (const std::exception &e)
    {
        error = e.what();
    }
    CHECK(error.find("fork_fault_b failed with an unknown error") != std::string::npos);
    CHECK(fork_test::running_modules() == 0);
    std::filesystem::remove_all(dir);
}

TEST_CASE("Two-module run: module 1 that fails with its DSP blocks running is stopped before it is destroyed")
{
    setup();
    std::string dir = temp_dir("m1_block");
    std::string input = write_input(dir, 256 * 1024);
    Pipeline p = make_pipeline({{"fail_after", 2}, {"with_block", true}}, nlohmann::json::object());

    // A demodulator starts its blocks in process() and stops them at its end. Failing in between,
    // it used to be destroyed with its blocks running, which still ended the program
    int before = fork_test::blocks_destroyed_running();
    std::string error;
    try
    {
        p.run(input, dir, nlohmann::json::object(), "baseband");
    }
    catch (const std::exception &e)
    {
        error = e.what();
    }
    CHECK(error.find("failure on purpose") != std::string::npos);
    CHECK(fork_test::blocks_destroyed_running().load() == before);
    CHECK(fork_test::running_modules() == 0);
    std::filesystem::remove_all(dir);
}

TEST_CASE("One module at a time: a module that fails with its DSP blocks running is stopped before it is destroyed")
{
    setup();
    std::string dir = temp_dir("single_block");
    std::string input = write_input(dir, 64 * 1024);
    Pipeline p = make_pipeline({{"fail_after", 2}, {"with_block", true}}, nlohmann::json::object());

    int before = fork_test::blocks_destroyed_running();
    std::string error;
    try
    {
        p.run(input, dir, {{"disable_multi_modules", true}}, "baseband");
    }
    catch (const std::exception &e)
    {
        error = e.what();
    }
    CHECK(error.find("failure on purpose") != std::string::npos);
    CHECK(fork_test::blocks_destroyed_running().load() == before);
    CHECK(fork_test::running_modules() == 0);
    std::filesystem::remove_all(dir);
}

TEST_CASE("Two-module run: modules with DSP blocks and no failure end cleanly")
{
    setup();
    std::string dir = temp_dir("good_block");
    std::string input = write_input(dir, 64 * 1024);
    Pipeline p = make_pipeline({{"with_block", true}}, {{"with_block", true}});

    int before = fork_test::blocks_destroyed_running();
    std::string final_file;
    CHECK_NOTHROW(p.run(input, dir, nlohmann::json::object(), "baseband", false, nullptr, nullptr, &final_file));
    CHECK(std::filesystem::file_size(final_file) == 64 * 1024);
    CHECK(fork_test::blocks_destroyed_running().load() == before);
    std::filesystem::remove_all(dir);
}

TEST_CASE("Two-module run: without failures the output is complete")
{
    setup();
    std::string dir = temp_dir("good");
    std::string input = write_input(dir, 256 * 1024);
    Pipeline p = make_pipeline({{"out_factor", 2}}, nlohmann::json::object());

    std::string final_file;
    CHECK_NOTHROW(p.run(input, dir, nlohmann::json::object(), "baseband", false, nullptr, nullptr, &final_file));
    REQUIRE(std::filesystem::exists(final_file));
    CHECK(std::filesystem::file_size(final_file) == 512 * 1024);
    std::filesystem::remove_all(dir);
}

TEST_CASE("Live pipeline: the last module failing is contained; the recording and the module before it keep going")
{
    setup();
    std::string dir = temp_dir("live_b");
    ctpl::thread_pool tp(4);
    LiveRig rig;
    LivePipeline lp(make_pipeline(nlohmann::json::object(), {{"fail_after", 3}}), nlohmann::json::object(), dir);
    lp.start(rig.live(), tp);
    Modules mods = lp.modules;
    REQUIRE(mods.size() == 2);

    // The failed module's input fills up in well under a second; it used to stall everything
    std::this_thread::sleep_for(1s);
    uint64_t rec1 = rig.recorded, a1 = bytes_in(mods[0]);
    std::this_thread::sleep_for(500ms);
    CHECK(rig.recorded > rec1);
    CHECK(bytes_in(mods[0]) > a1);

    nlohmann::json stats = lp.getModulesStats();
    CHECK(stats["fork_fault_b"].contains("error"));
    CHECK(!stats["fork_fault_a"].contains("error"));

    StopResult r = stop_live(lp, mods, 5000ms);
    CHECK(r.in_time);
    CHECK(r.seconds < 2.0);
    CHECK(!r.threw);
    CHECK(fork_test::running_modules() == 0);
    std::filesystem::remove_all(dir);
}

TEST_CASE("Live pipeline: the first module failing does not stall the recording")
{
    setup();
    std::string dir = temp_dir("live_a");
    ctpl::thread_pool tp(4);
    LiveRig rig;
    LivePipeline lp(make_pipeline({{"fail_after", 3}}, nlohmann::json::object()), nlohmann::json::object(), dir);
    lp.start(rig.live(), tp);
    Modules mods = lp.modules;
    REQUIRE(mods.size() == 2);

    std::this_thread::sleep_for(500ms);
    uint64_t rec1 = rig.recorded;
    std::this_thread::sleep_for(500ms);
    CHECK(rig.recorded > rec1);

    nlohmann::json stats = lp.getModulesStats();
    CHECK(stats["fork_fault_a"].contains("error"));

    // stop() used to throw here, and leave the second module running
    StopResult r = stop_live(lp, mods, 5000ms);
    CHECK(r.in_time);
    CHECK(r.seconds < 2.0);
    CHECK(!r.threw);
    CHECK(fork_test::running_modules() == 0);
    std::filesystem::remove_all(dir);
}

TEST_CASE("Live pipeline: a module that cannot be set up leaves nothing running")
{
    setup();
    std::string dir = temp_dir("live_init");
    ctpl::thread_pool tp(4);
    LiveRig rig;
    LivePipeline lp(make_pipeline(nlohmann::json::object(), {{"fail_init", true}}), nlohmann::json::object(), dir);
    CHECK_THROWS(lp.start(rig.live(), tp));
    Modules mods = lp.modules;

    // The first module used to be started before the second one was set up, and kept running
    std::this_thread::sleep_for(300ms);
    CHECK(fork_test::running_modules() == 0);

    // Stopping after a failed start is safe
    StopResult r = stop_live(lp, mods, 5000ms);
    CHECK(r.in_time);
    CHECK(!r.threw);
    CHECK(fork_test::running_modules() == 0);
    std::filesystem::remove_all(dir);
}

TEST_CASE("Live pipeline: a pipeline without live modules is refused")
{
    setup();
    std::string dir = temp_dir("live_none");
    ctpl::thread_pool tp(2);
    Pipeline p = make_pipeline(nlohmann::json::object(), nlohmann::json::object());
    p.live_cfg.normal_live = {};
    LivePipeline lp(p, nlohmann::json::object(), dir);
    // This used to read past the end of the (empty) module list
    CHECK_THROWS(lp.start(std::make_shared<dsp::stream<complex_t>>(), tp));
    CHECK(lp.getOutputFile() == "");
    std::filesystem::remove_all(dir);
}

TEST_CASE("Live pipeline: modules still running are stopped when the pipeline goes away")
{
    setup();
    std::string dir = temp_dir("live_dtor");
    ctpl::thread_pool tp(4);
    LiveRig rig;
    Modules mods;
    {
        LivePipeline lp(make_pipeline(nlohmann::json::object(), nlohmann::json::object()), nlohmann::json::object(), dir);
        lp.start(rig.live(), tp);
        mods = lp.modules;
        CHECK(wait_for([]() { return fork_test::running_modules() == 2; }, 2000ms));
    }
    CHECK(wait_for([]() { return fork_test::running_modules() == 0; }, 2000ms));
    rescue(mods);
    CHECK(wait_for([]() { return fork_test::running_modules() == 0; }, 5000ms));
    std::filesystem::remove_all(dir);
}

namespace
{
    struct FailingHandler : public satdump::handlers::ProcessingHandler
    {
        bool other = false;
        void do_process() override
        {
            if (other)
                throw 42;
            throw std::runtime_error("processing failure on purpose");
        }
        ~FailingHandler()
        {
            if (async_thread.joinable())
                async_thread.join();
        }
    };

    std::atomic<int> done_events(0);
} // namespace

TEST_CASE("Processing handler: processing that fails in the background is logged, not fatal")
{
    for (bool other : {false, true})
    {
        FailingHandler h;
        h.other = other;
        h.asyncProcess(); // The program used to end here (std::terminate)
        CHECK(wait_for([&]() { return !h.get_is_processing(); }, 5000ms));
    }
}

TEST_CASE("Processing handler: processing that fails clears the busy flag and still reports the error")
{
    FailingHandler h;
    CHECK_THROWS(h.process());
    CHECK(!h.get_is_processing());
}

TEST_CASE("Offline processing: an output folder that cannot be made is logged, and processing still ends")
{
    setup();
    static bool registered = false;
    if (!registered)
    {
        registered = true;
        satdump::eventBus->register_handler<satdump::SetIsDoneProcessingEvent>([](satdump::SetIsDoneProcessingEvent) { done_events++; });
    }

    std::string dir = temp_dir("offproc");
    std::string input = write_input(dir, 4096);
    int before = done_events;
    {
        // A folder inside a regular file cannot be made; that used to end the program
        satdump::handlers::OffProcessingHandler h(make_pipeline(nlohmann::json::object(), nlohmann::json::object()), "baseband", input, input + "/out",
                                                  nlohmann::json::object());
        CHECK(wait_for([&]() { return done_events > before; }, 5000ms));
    }
    CHECK(fork_test::running_modules() == 0);
    std::filesystem::remove_all(dir);
}

TEST_CASE("Task queue: a task that fails does not stop the tasks after it")
{
    std::atomic<bool> ran(false);
    {
        satdump::TaskQueue q;
        q.push([]() { throw std::runtime_error("task failure on purpose"); });
        q.push([]() { throw 42; }); // This one used to end the program
        q.push([&]() { ran = true; });
        CHECK(wait_for([&]() { return (bool)ran; }, 5000ms));
    }
    CHECK(ran);
}

TEST_CASE("Task queue: a task pushed as the queue's thread ends still runs")
{
    // The thread used to mark itself as ended after letting go of the lock: a task pushed
    // in between was left in the queue. Each task is pushed the moment the one before has run, to
    // land in that gap as often as possible
    std::atomic<int> count(0);
    satdump::TaskQueue q;
    int lost = -1;
    for (int i = 0; i < 20000 && lost < 0; i++)
    {
        q.push([&]() { count++; });
        auto start = std::chrono::steady_clock::now();
        while (count != i + 1 && lost < 0)
        {
            if (std::chrono::steady_clock::now() - start > 2s)
                lost = i;
            std::this_thread::yield();
        }
    }
    CHECK(lost == -1);
}

namespace
{
    struct FailingScheduledEvent
    {
    };
    struct FailingOtherScheduledEvent
    {
    };
    struct GoodScheduledEvent
    {
    };
    std::atomic<int> failing_runs(0), failing_other_runs(0), good_runs(0);
} // namespace

TEST_CASE("Task scheduler: a task that throws is logged, and the tasks after it still run")
{
    setup();
    static bool registered = false;
    if (!registered)
    {
        registered = true;
        satdump::eventBus->register_handler<FailingScheduledEvent>(
            [](FailingScheduledEvent)
            {
                failing_runs++;
                throw std::runtime_error("scheduled task failure on purpose"); // This used to end the program
            });
        satdump::eventBus->register_handler<FailingOtherScheduledEvent>(
            [](FailingOtherScheduledEvent)
            {
                failing_other_runs++;
                throw 42;
            });
        satdump::eventBus->register_handler<GoodScheduledEvent>([](GoodScheduledEvent) { good_runs++; });
    }

    {
        satdump::TaskScheduler ts;
        ts.start_thread();
        const time_t now = time(nullptr);
        // All due now; they run in name order, so the failing ones run first
        ts.add_task("a_failing", std::make_shared<FailingScheduledEvent>(), now - 3600, 3600);
        ts.add_task("b_failing_other", std::make_shared<FailingOtherScheduledEvent>(), now - 3600, 3600);
        ts.add_task("c_good", std::make_shared<GoodScheduledEvent>(), now - 3600, 3600);
        CHECK(wait_for([]() { return good_runs > 0; }, 5000ms));
        std::this_thread::sleep_for(300ms);
    }

    // Each ran once: a task that failed waits for its next run like any other
    CHECK(failing_runs == 1);
    CHECK(failing_other_runs == 1);
    CHECK(good_runs == 1);
}
