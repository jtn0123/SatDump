// Fork: exercise the real blocks' extra workers, with bounded waits and controlled overlap.
#include "../../plugins/dvb_support/dvbs2/dvbs2_pl_sync.h"
#include "dsp/utils/reshape_buffer.h"
#include "dsp/legacy/module_wrapper.h"
#include "doctest.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <exception>
#include <stdexcept>
#include <mutex>
#include <thread>

namespace
{
    using namespace satdump::ndsp;

    bool within(const std::function<bool()> &condition, int seconds = 10)
    {
        auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        while (!condition())
        {
            if (std::chrono::steady_clock::now() >= end)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

    // Fork: a stuck join cannot be unwound safely; fail the process instead of hanging ctest.
    void bounded(const std::function<void()> &fn)
    {
        std::atomic<bool> done{false};
        std::exception_ptr error;
        std::thread runner([&]
        {
            try { fn(); }
            catch (...) { error = std::current_exception(); }
            done = true;
        });
        if (!within([&] { return done.load(); }, 60))
        {
            std::fprintf(stderr, "extra DSP workers: lifecycle did not finish within 60 s\n");
            std::_Exit(1);
        }
        runner.join();
        if (error)
            std::rethrow_exception(error);
    }

    // Fork: these new-DSP fixtures run the real extra workers while the main worker only
    // waits for stop(true). This separates thread ownership from FIFO scheduling.
    class ReshapeFixture : public ReshapeBufferBlock<float>
    {
        bool work() override
        {
            std::this_thread::yield();
            return work_should_exit;
        }
    public:
        ReshapeFixture() { outputs[0].fifo = std::make_shared<DSPStream>(4); }
        void stop(bool stop_now = true, bool force = false) override { ReshapeBufferBlock<float>::stop(stop_now, force); }
    };

    class WaitingModule : public satdump::pipeline::ProcessingModule
    {
    public:
        static std::atomic<int> running;
        WaitingModule() : ProcessingModule("", "", nlohmann::json::object()) {}
        std::vector<satdump::pipeline::ModuleDataType> getInputTypes() override { return {satdump::pipeline::DATA_STREAM}; }
        std::vector<satdump::pipeline::ModuleDataType> getOutputTypes() override { return {satdump::pipeline::DATA_STREAM}; }
        void process() override
        {
            running++;
            uint8_t byte;
            while (input_fifo->read(&byte, 1) > 0) {}
            running--;
        }
        void drawUI(bool) override {}
        std::string getIDM() override { return "extra_worker_test"; }
    };
    std::atomic<int> WaitingModule::running{0};

    class ModuleFixture : public ModuleWrapperBlock
    {
        bool work() override
        {
            std::this_thread::yield();
            return work_should_exit;
        }
    public:
        ModuleFixture()
        {
            module_id = "extra_worker_test";
            outputs[0].fifo = std::make_shared<DSPStream>(4);
        }
        void stop(bool stop_now = true, bool force = false) override { ModuleWrapperBlock::stop(stop_now, force); }
    };

    class S2Fixture : public dvbs2::S2PLSyncBlock
    {
    public:
        S2Fixture() : S2PLSyncBlock(std::make_shared<dsp::stream<complex_t>>(512), 1, false)
        {
            output_stream = std::make_shared<dsp::stream<complex_t>>(512);
        }
    };

    template <typename T> class Probe : public T
    {
        bool live = false;
    protected:
        void start_extra() override
        {
            if (on_start)
                on_start();
            T::start_extra();
            live = true;
            starts++;
            if (fail_start)
                throw std::runtime_error("extra worker start failed on purpose");
        }
        void stop_extra() noexcept override
        {
            if (on_stop)
                on_stop();
            T::stop_extra();
            if (live)
            {
                live = false;
                joins++;
            }
        }
    public:
        std::function<void()> on_start, on_stop;
        bool fail_start = false;
        std::atomic<int> starts{0}, joins{0};
    };

    template <typename T> void lifecycle()
    {
        Probe<T> block;
        block.stop();
        block.stop();
        CHECK(block.starts == 0);
        CHECK(block.joins == 0);
        block.start();
        CHECK_THROWS(block.start());
        CHECK(block.starts == 1);
        block.stop();
        block.stop();
        CHECK(block.starts == block.joins);
        block.start();
        block.stop();
        CHECK(block.starts == 2);
        CHECK(block.joins == 2);

        block.fail_start = true;
        CHECK_THROWS_WITH(block.start(), "extra worker start failed on purpose");
        CHECK(block.starts == block.joins);
        block.stop();
        block.fail_start = false;
        block.start();
        block.stop();
        CHECK(block.starts == block.joins);
    }

    template <typename T> void overlap()
    {
        Probe<T> block;
        std::atomic<bool> held{false}, stopping{false}, returned{false};
        bool slipped = false;
        block.on_start = [&]
        {
            held = true;
            if (!within([&] { return stopping.load(); }))
                throw std::runtime_error("stopper was not scheduled");
            // Fork: this is a negative assertion; the timeout only widens the old race.
            auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
            while (!returned && std::chrono::steady_clock::now() < end)
                std::this_thread::yield();
            slipped = returned;
        };
        std::exception_ptr start_error, stop_error;
        std::thread starter([&]
        {
            try { block.start(); }
            catch (...) { start_error = std::current_exception(); }
        });
        CHECK(within([&] { return held.load(); }));
        std::thread stopper([&]
        {
            stopping = true;
            try { block.stop(); }
            catch (...) { stop_error = std::current_exception(); }
            returned = true;
        });
        starter.join();
        stopper.join();
        CHECK_FALSE(start_error);
        CHECK_FALSE(stop_error);
        CHECK_FALSE(slipped);
        CHECK(block.starts == 1);
        CHECK(block.joins == 1);
        block.stop();
    }

    template <typename T> void stop_overlap()
    {
        Probe<T> block;
        block.start();
        std::atomic<bool> held{false}, starting{false}, returned{false};
        bool slipped = false;
        block.on_stop = [&]
        {
            held = true;
            if (!within([&] { return starting.load(); }))
                std::_Exit(1);
            auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
            while (!returned && std::chrono::steady_clock::now() < end)
                std::this_thread::yield();
            slipped = returned;
        };
        std::exception_ptr start_error, stop_error;
        std::thread stopper([&]
        {
            try { block.stop(); }
            catch (...) { stop_error = std::current_exception(); }
        });
        CHECK(within([&] { return held.load(); }));
        std::thread starter([&]
        {
            starting = true;
            try { block.start(); }
            catch (...) { start_error = std::current_exception(); }
            returned = true;
        });
        stopper.join();
        starter.join();
        CHECK_FALSE(start_error);
        CHECK_FALSE(stop_error);
        CHECK_FALSE(slipped);
        CHECK(block.starts == 2);
        CHECK(block.joins == 1);
        block.on_stop = nullptr;
        block.stop();
        CHECK(block.starts == block.joins);
    }

    template <typename T> void stress()
    {
        Probe<T> block;
        const int rounds = 100;
        for (int i = 0; i < rounds; ++i)
        {
            std::atomic<int> ready{0};
            std::atomic<bool> go{false};
            std::exception_ptr a_error, b_error;
            auto call = [&](std::exception_ptr &error, bool start)
            {
                ready++;
                while (!go)
                    std::this_thread::yield();
                try
                {
                    if (start) block.start();
                    else block.stop();
                }
                catch (...) { error = std::current_exception(); }
            };
            // Fork: include simultaneous stops, so joining the same worker twice is tested.
            std::thread a([&] { call(a_error, i % 2 == 0); });
            std::thread b([&] { call(b_error, false); });
            while (ready < 2)
                std::this_thread::yield();
            go = true;
            a.join();
            b.join();
            CHECK_FALSE(a_error);
            CHECK_FALSE(b_error);
            block.stop();
            CHECK(block.starts == block.joins);
        }
        CHECK(block.starts == rounds / 2);
    }

    void register_module()
    {
        static bool done = false;
        if (!done)
        {
            satdump::pipeline::modules_registry.push_back({"extra_worker_test", nlohmann::json::object(),
                [](std::string, std::string, nlohmann::json)
                { return std::make_shared<WaitingModule>(); }});
            done = true;
        }
    }
}

TEST_CASE("extra DSP workers: DVB-S2 lifecycle, overlap and two-thread stress")
{
    bounded([] { lifecycle<S2Fixture>(); overlap<S2Fixture>(); stop_overlap<S2Fixture>(); stress<S2Fixture>(); });
}

TEST_CASE("extra DSP workers: reshape lifecycle, overlap and two-thread stress")
{
    bounded([] { lifecycle<ReshapeFixture>(); overlap<ReshapeFixture>(); stop_overlap<ReshapeFixture>(); stress<ReshapeFixture>(); });
}

TEST_CASE("extra DSP workers: module wrapper lifecycle, overlap and two-thread stress")
{
    register_module();
    bounded([] { lifecycle<ModuleFixture>(); overlap<ModuleFixture>(); stop_overlap<ModuleFixture>(); stress<ModuleFixture>(); });
    CHECK(WaitingModule::running == 0);
}

TEST_CASE("extra DSP workers: DVB-S2 restart still passes complete frames")
{
    bounded([]
    {
        S2Fixture block;
        for (int run = 0; run < 3; ++run)
        {
            // Fork: stream owners reopen external streams; the block resets its internal ring.
            block.input_stream->clearReadStop();
            block.output_stream->clearWriteStop();
            block.start();
            for (int i = 0; i < block.raw_frame_size; ++i)
                block.input_stream->writeBuf[i] = complex_t(run + 1, 0);
            CHECK(block.input_stream->swap(block.raw_frame_size));
            CHECK(block.output_stream->read() == block.raw_frame_size);
            CHECK(block.output_stream->readBuf[0].real == run + 1);
            block.output_stream->flush();
            block.stop();
        }
    });
}

TEST_CASE("extra DSP workers: real reshape transfers samples after restart")
{
    bounded([]
    {
        ReshapeBufferBlock<float> block;
        block.set_cfg("buffer_size", 32);
        auto input = std::make_shared<DSPStream>(4);
        auto output = block.get_output(0, 4).fifo;
        block.set_input({"in", DSP_SAMPLE_TYPE_F32, input}, 0);
        for (int run = 0; run < 3; ++run)
        {
            block.start();
            auto samples = input->newBufferSamples(32, sizeof(float));
            samples.size = 32;
            for (int i = 0; i < 32; ++i)
                samples.getSamples<float>()[i] = run + 1;
            input->wait_enqueue(samples);
            auto got = output->wait_dequeue();
            CHECK(got.size == 32);
            CHECK(got.getSamples<float>()[0] == run + 1);
            output->free(got);
            input->wait_enqueue(input->newBufferTerminator());
            got = output->wait_dequeue();
            CHECK(got.isTerminator());
            output->free(got);
            block.stop();
            block.stop();
        }
    });
}

TEST_CASE("extra DSP workers: real module wrapper ends and restarts")
{
    register_module();
    bounded([]
    {
        ModuleWrapperBlock block;
        block.module_id = "extra_worker_test";
        auto input = std::make_shared<DSPStream>(4);
        auto output = block.get_output(0, 4).fifo;
        block.set_input({"in", DSP_SAMPLE_TYPE_S8, input}, 0);
        block.stop();
        for (int run = 0; run < 3; ++run)
        {
            block.start();
            CHECK(within([] { return WaitingModule::running.load() == 1; }));
            input->wait_enqueue(input->newBufferTerminator());
            auto got = output->wait_dequeue();
            CHECK(got.isTerminator());
            output->free(got);
            block.stop();
            block.stop();
            CHECK(WaitingModule::running == 0);
        }
    });
}

TEST_CASE("extra DSP workers: DVB-S2 start waits for the second worker join")
{
    // Fork: hold the actual second worker, not just the base worker or lifecycle hook.
    class JoinGatedS2 : public S2Fixture
    {
        void work2() override
        {
            active++;
            entered = true;
            while (!release)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            active--;
        }
    public:
        std::atomic<bool> entered{false}, release{false};
        std::atomic<int> active{0};
    };
    bounded([]
    {
        Probe<JoinGatedS2> block;
        block.start();
        CHECK(within([&] { return block.entered.load(); }));
        std::atomic<bool> stopping{false}, starting{false}, returned{false};
        block.on_stop = [&] { stopping = true; };
        std::exception_ptr stop_error, start_error;
        std::thread stopper([&]
        {
            try { block.stop(); }
            catch (...) { stop_error = std::current_exception(); }
        });
        CHECK(within([&] { return stopping.load(); }));
        std::thread starter([&]
        {
            starting = true;
            try { block.start(); }
            catch (...) { start_error = std::current_exception(); }
            returned = true;
        });
        CHECK(within([&] { return starting.load(); }));
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
        while (!returned && std::chrono::steady_clock::now() < end)
            std::this_thread::yield();
        bool slipped = returned;
        block.release = true;
        stopper.join();
        starter.join();
        CHECK_FALSE(start_error);
        CHECK_FALSE(stop_error);
        CHECK_FALSE(slipped);
        block.on_stop = nullptr;
        block.stop();
        CHECK(block.active == 0);
        CHECK(block.starts == 2);
        CHECK(block.joins == 2);
    });
}
