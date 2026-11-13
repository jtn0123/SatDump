// Fork: exercise the real blocks' extra workers, with bounded waits and controlled overlap.
#include "../../plugins/dvb_support/dvbs2/dvbs2_pl_sync.h"
#include "dsp/utils/reshape_buffer.h"
#include "dsp/legacy/module_wrapper.h"
#include "doctest.h"
#include "block_lifecycle_test_access.h"
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
        std::mutex &lifecycle_mutex() { return BlockLifecycleTestAccess::mutex(*this); }
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
        std::mutex &lifecycle_mutex() { return BlockLifecycleTestAccess::mutex(*this); }
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
        std::mutex &lifecycle_mutex() { return dsp::BlockLifecycleTestAccess::mutex(*this); }
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

    template <typename T> void hooks_hold_mutex()
    {
        Probe<T> block;
        std::atomic<bool> held{false}, release{false};
        auto hold = [&]
        {
            held = true;
            if (!within([&] { return release.load(); }))
                std::_Exit(1);
        };
        auto check = [&](const std::function<void()> &operation)
        {
            held = false;
            release = false;
            std::exception_ptr error;
            std::thread worker([&]
            {
                try { operation(); }
                catch (...) { error = std::current_exception(); }
            });
            CHECK(within([&] { return held.load(); }));
            // Fork: actually attempt the lock on this thread while the hook is held.
            // The worker cannot release its hook until this assertion has run.
            {
                std::unique_lock<std::mutex> attempt(block.lifecycle_mutex(), std::try_to_lock);
                CHECK_FALSE(attempt.owns_lock());
            }
            release = true;
            worker.join();
            CHECK_FALSE(error);
        };
        block.on_start = hold;
        check([&] { block.start(); });
        block.on_start = nullptr;
        block.on_stop = hold;
        check([&] { block.stop(); });
        block.on_stop = nullptr;
        CHECK(block.starts == 1);
        CHECK(block.joins == 1);
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

TEST_CASE("extra DSP workers: DVB-S2 lifecycle and two-thread stress")
{
    bounded([] { lifecycle<S2Fixture>(); stress<S2Fixture>(); });
}

TEST_CASE("extra DSP workers: reshape lifecycle and two-thread stress")
{
    bounded([] { lifecycle<ReshapeFixture>(); stress<ReshapeFixture>(); });
}

TEST_CASE("extra DSP workers: module wrapper lifecycle and two-thread stress")
{
    register_module();
    bounded([] { lifecycle<ModuleFixture>(); stress<ModuleFixture>(); });
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

TEST_CASE("extra DSP workers: reshape drains queued chunks before EOF and restart")
{
    bounded([]
    {
        ReshapeBufferBlock<float> block;
        block.set_cfg("buffer_size", 32);
        auto input = std::make_shared<DSPStream>(4);
        auto output = block.get_output(0, 4).fifo;
        block.set_input({"in", DSP_SAMPLE_TYPE_F32, input}, 0);
        const int lengths[] = {99, 64, 64, 31, 0, 95};
        for (int run = 0; run < 6; ++run)
        {
            const int chunk_size = run == 1 ? 16 : 32;
            block.set_cfg("buffer_size", chunk_size);
            // Fork: hold every output allocation until the input worker has consumed EOF.
            // This proves it can finish without publishing output, even under backpressure.
            std::vector<DSPBuffer> held;
            for (int i = 0; i < 4; ++i)
                held.push_back(output->alloc(0));
            block.start();
            int count = lengths[run];
            if (count > 0)
            {
                auto samples = input->newBufferSamples(count, sizeof(float));
                samples.size = count;
                for (int i = 0; i < count; ++i)
                    samples.getSamples<float>()[i] = run * 1000 + i;
                input->wait_enqueue(samples);
            }
            bool propagate = run != 5;
            input->wait_enqueue(input->newBufferTerminator(propagate));
            CHECK(within([&] { return BlockLifecycleTestAccess::main_done(block); }));
            for (auto &buffer : held)
                output->free(buffer);
            int received = 0;
            if (!propagate)
                block.stop(); // Fork: all complete output is now queued; no EOF is forwarded.
            while (true)
            {
                DSPBuffer got;
                if (propagate)
                    got = output->wait_dequeue();
                else if (!output->try_dequeue(got))
                    break;
                bool ended = got.isTerminator();
                if (!propagate)
                    CHECK_FALSE(ended);
                if (!ended)
                {
                    CHECK(got.size == static_cast<uint32_t>(chunk_size));
                    for (uint32_t i = 0; i < got.size; ++i)
                        CHECK(got.getSamples<float>()[i] == run * 1000 + received + i);
                    received += got.size;
                }
                output->free(got);
                if (ended)
                    break;
            }
            block.stop();
            block.stop();
            CHECK(received == (count / chunk_size) * chunk_size);
            DSPBuffer leftover;
            CHECK_FALSE(output->try_dequeue(leftover));
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

TEST_CASE("extra DSP workers: DVB-S2 second worker join holds the lifecycle mutex")
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
        std::atomic<bool> stopping{false};
        block.on_stop = [&] { stopping = true; };
        std::exception_ptr stop_error;
        std::thread stopper([&]
        {
            try { block.stop(); }
            catch (...) { stop_error = std::current_exception(); }
        });
        CHECK(within([&] { return stopping.load(); }));
        {
            std::unique_lock<std::mutex> attempt(block.lifecycle_mutex(), std::try_to_lock);
            CHECK_FALSE(attempt.owns_lock());
        }
        CHECK(block.active == 1);
        block.release = true;
        stopper.join();
        CHECK_FALSE(stop_error);
        block.start();
        block.on_stop = nullptr;
        block.stop();
        CHECK(block.active == 0);
        CHECK(block.starts == 2);
        CHECK(block.joins == 2);
    });
}

TEST_CASE_TEMPLATE("extra DSP workers: lifecycle hooks hold the mutex", T, S2Fixture, ReshapeFixture, ModuleFixture)
{
    register_module();
    bounded([] { hooks_hold_mutex<T>(); });
}

TEST_CASE("extra DSP workers: reshape rejects nonpositive sizes and accepts one")
{
    bounded([]
    {
        ReshapeBufferBlock<float> block;
        CHECK(block.set_cfg("buffer_size", 32) == Block::RES_OK);
        for (int invalid : {0, -1, -1024})
        {
            CHECK(block.set_cfg("buffer_size", invalid) == Block::RES_ERR);
            CHECK(block.get_cfg("buffer_size") == 32);
        }
        CHECK(block.set_cfg("buffer_size", 1) == Block::RES_OK);
        auto input = std::make_shared<DSPStream>(4);
        auto output = block.get_output(0, 4).fifo;
        block.set_input({"in", DSP_SAMPLE_TYPE_F32, input}, 0);
        block.start();
        auto samples = input->newBufferSamples(3, sizeof(float));
        samples.size = 3;
        for (int i = 0; i < 3; ++i)
            samples.getSamples<float>()[i] = i + 1;
        input->wait_enqueue(samples);
        input->wait_enqueue(input->newBufferTerminator());
        for (int i = 0; i < 3; ++i)
        {
            auto got = output->wait_dequeue();
            CHECK_FALSE(got.isTerminator());
            CHECK(got.size == 1);
            CHECK(got.getSamples<float>()[0] == i + 1);
            output->free(got);
        }
        auto end = output->wait_dequeue();
        CHECK(end.isTerminator());
        output->free(end);
        block.stop();
        CHECK_FALSE(BlockLifecycleTestAccess::main_joinable(block));
        CHECK_FALSE(ReshapeBufferTestAccess::extra_joinable(block));
    });
}

TEST_CASE("extra DSP workers: reshape invalid startup creates no workers")
{
    // Fork: a sentinel catches an invalid start before a broken implementation can spin
    // forever with zero-sized output. Valid production startup still uses the real workers.
    class InvalidStartProbe : public ReshapeFixture
    {
        void start_extra() override
        {
            starts++;
            throw std::runtime_error("invalid state reached worker startup");
        }
    public:
        int starts = 0;
    };
    bounded([]
    {
        for (int invalid : {0, -1})
        {
            InvalidStartProbe block;
            ReshapeBufferTestAccess::force_size(block, invalid);
            CHECK_THROWS_WITH(block.start(), doctest::Contains("Reshape buffer size must be greater than zero"));
            CHECK(block.starts == 0);
            CHECK_FALSE(BlockLifecycleTestAccess::main_joinable(block));
            CHECK_FALSE(ReshapeBufferTestAccess::extra_joinable(block));
            block.stop();
            block.stop();
        }
    });
}

TEST_CASE("extra DSP workers: reshape live shrink and growth respect chunk boundaries")
{
    bounded([]
    {
        for (const auto &sizes : {std::pair<int, int>{32, 16}, {16, 64}})
        {
            const int old_size = sizes.first, new_size = sizes.second;
            CAPTURE(old_size);
            CAPTURE(new_size);
            ReshapeBufferBlock<float> block;
            CHECK(block.set_cfg("buffer_size", old_size) == Block::RES_OK);
            auto input = std::make_shared<DSPStream>(4);
            auto output = block.get_output(0, 4).fifo;
            block.set_input({"in", DSP_SAMPLE_TYPE_F32, input}, 0);
            std::vector<DSPBuffer> held;
            for (int i = 0; i < 4; ++i)
                held.push_back(output->alloc(0));
            auto send = [&](int offset, int count)
            {
                auto samples = input->newBufferSamples(count, sizeof(float));
                samples.size = count;
                for (int i = 0; i < count; ++i)
                    samples.getSamples<float>()[i] = offset + i;
                input->wait_enqueue(samples);
            };
            block.start();
            send(0, old_size + 1);
            // Fork: exactly one sample remains only after the worker staged its first old-size
            // chunk. Holding all output allocations keeps it at that boundary until release.
            CHECK(within([&] { return ReshapeBufferTestAccess::readable(block) == 1; }));
            CHECK(block.set_cfg("buffer_size", new_size) == Block::RES_OK);
            CHECK(block.get_cfg("buffer_size") == new_size);
            send(old_size + 1, 2 * new_size - 1);
            input->wait_enqueue(input->newBufferTerminator());
            for (auto &buffer : held)
                output->free(buffer);
            std::vector<uint32_t> chunks;
            int received = 0;
            while (true)
            {
                auto got = output->wait_dequeue();
                bool ended = got.isTerminator();
                if (!ended)
                {
                    chunks.push_back(got.size);
                    for (uint32_t i = 0; i < got.size; ++i)
                        CHECK(got.getSamples<float>()[i] == received + i);
                    received += got.size;
                }
                output->free(got);
                if (ended)
                    break;
            }
            block.stop();
            const std::vector<uint32_t> expected{static_cast<uint32_t>(old_size), static_cast<uint32_t>(new_size), static_cast<uint32_t>(new_size)};
            CHECK(chunks == expected);
            CHECK(received == old_size + 2 * new_size);
            CHECK(ReshapeBufferTestAccess::staging_size(block) >= static_cast<size_t>(new_size));
            DSPBuffer leftover;
            CHECK_FALSE(output->try_dequeue(leftover));
        }
    });
}

TEST_CASE("extra DSP workers: reshape staging allocation failure is contained")
{
    bounded([]
    {
        ReshapeBufferBlock<float> block;
        block.set_cfg("buffer_size", 32);
        auto input = std::make_shared<DSPStream>(4);
        auto output = block.get_output(0, 4).fifo;
        block.set_input({"in", DSP_SAMPLE_TYPE_F32, input}, 0);
        std::vector<DSPBuffer> held;
        for (int i = 0; i < 4; ++i)
            held.push_back(output->alloc(0));
        block.start();
        auto samples = input->newBufferSamples(33, sizeof(float));
        samples.size = 33;
        for (int i = 0; i < 33; ++i)
            samples.getSamples<float>()[i] = i;
        input->wait_enqueue(samples);
        CHECK(within([&] { return ReshapeBufferTestAccess::readable(block) == 1; }));
        // Fork: bypass the validated setter to force vector::resize to throw length_error
        // at the next boundary, without exhausting memory or adding a production test hook.
        ReshapeBufferTestAccess::force_size(block, -1);
        input->wait_enqueue(input->newBufferTerminator());
        for (auto &buffer : held)
            output->free(buffer);
        auto first = output->wait_dequeue();
        CHECK(first.size == 32);
        output->free(first);
        auto end = output->wait_dequeue();
        CHECK(end.isTerminator());
        output->free(end);
        block.stop();
        CHECK(block.has_failed());
        CHECK_FALSE(block.get_failure().empty());
        CHECK_FALSE(BlockLifecycleTestAccess::main_joinable(block));
        CHECK_FALSE(ReshapeBufferTestAccess::extra_joinable(block));
    });
}
