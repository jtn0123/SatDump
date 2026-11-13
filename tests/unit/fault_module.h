#pragma once

// Fork test support: a pipeline module that fails on purpose. Used by the unit tests in
// test_thread_faults.cpp and, through fault_plugin.cpp, by tests/cli/thread_faults.sh.
//
// Parameters:
//   fail_after  chunks to process before throwing (-1, the default: never; 0: at once)
//   fail_init   true to throw from init()
// Fork: fail_running throws on the live command thread after the radio and pipeline start;
// with_block also checks that the module's block has started before that error is raised.
//   fail_with   "std" (the default) throws a std::exception, "other" throws an int
//   out_factor  bytes written per byte read (default 1)
//   with_block  true to start a DSP block in process() and stop it in stop(), as demodulators do
//   block_fails true for a DSP block (as with_block) whose work() throws at once; the module
//               waits until it has failed, then carries on, as a demodulator does when one of
//               its blocks failed

#include "common/dsp/block.h"
#include "core/exception.h"
#include "core/plugin.h"
#include "live_running_event.h"
#include "logger.h"
#include "pipeline/module.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <condition_variable>
#include <mutex>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace fork_test
{
    // How many FaultModule::process() calls are running right now
    inline std::atomic<int> &running_modules()
    {
        static std::atomic<int> count(0);
        return count;
    }

    // How many FaultBlocks were destroyed while still running (nobody had stopped them)
    inline std::atomic<int> &blocks_destroyed_running()
    {
        static std::atomic<int> count(0);
        return count;
    }

    // A DSP block like the ones a demodulator starts in process(); it waits for samples that never
    // come until it is stopped
    class FaultBlock : public dsp::Block<complex_t, complex_t>
    {
    private:
        void work() override
        {
            if (fail)
                throw satdump_exception("block failure on purpose");
            if (input_stream->read() > 0)
                input_stream->flush();
        }

    public:
        bool fail = false; // work() throws at once

        FaultBlock() : dsp::Block<complex_t, complex_t>(std::make_shared<dsp::stream<complex_t>>(4096)) {}

        ~FaultBlock()
        {
            // Left to the base destructor, a running block is stopped once this part is gone, and
            // its thread can then end the program ("pure virtual method called"). It is counted
            // and stopped here instead, so a test can check for it without crashing
            if (should_run)
            {
                blocks_destroyed_running()++;
                logger->error("Fault test block destroyed while running");
                stop();
            }
        }
    };

    class FaultModule : public satdump::pipeline::ProcessingModule
    {
    private:
        std::string d_id;
        int d_fail_after = -1;
        bool d_fail_init = false;
        std::string d_fail_with = "std";
        int d_out_factor = 1;
        bool d_with_block = false;
        bool d_block_fails = false;
        // Fork: a held consumer and a byte-preserving writer for CLI regressions.
        bool d_hold_until_stop = false;
        bool d_preserve_input = false;
        bool d_stopped = false;
        std::mutex d_stop_mutex;
        std::condition_variable d_stop_cv;
        // Fork: shared with the event handler, so it never holds a dangling module pointer.
        bool d_fail_running = false;
        std::shared_ptr<std::atomic<bool>> d_running_ready = std::make_shared<std::atomic<bool>>(false);
        std::shared_ptr<FaultBlock> d_block;

        struct Running
        {
            Running() { running_modules()++; }
            ~Running() { running_modules()--; }
        };

        void fail()
        {
            if (d_fail_with == "other")
                throw 42;
            throw satdump_exception("failure on purpose");
        }

    public:
        std::atomic<uint64_t> bytes_in{0}, bytes_out{0};

        FaultModule(std::string id, std::string input_file, std::string output_file_hint, nlohmann::json parameters)
            : ProcessingModule(input_file, output_file_hint, parameters), d_id(id)
        {
            d_hold_until_stop = parameters.value("hold_until_stop", false);
            d_preserve_input = parameters.value("preserve_input", false);
            if (parameters.contains("fail_after"))
                d_fail_after = parameters["fail_after"].get<int>();
            if (parameters.contains("fail_running"))
                d_fail_running = parameters["fail_running"].get<bool>();
            if (parameters.contains("fail_init"))
                d_fail_init = parameters["fail_init"].get<bool>();
            if (parameters.contains("fail_with"))
                d_fail_with = parameters["fail_with"].get<std::string>();
            if (parameters.contains("out_factor"))
                d_out_factor = parameters["out_factor"].get<int>();
            if (parameters.contains("with_block"))
                d_with_block = parameters["with_block"].get<bool>();
            if (parameters.contains("block_fails"))
                d_block_fails = parameters["block_fails"].get<bool>();
        }

        std::vector<satdump::pipeline::ModuleDataType> getInputTypes() override { return {satdump::pipeline::DATA_FILE, satdump::pipeline::DATA_STREAM, satdump::pipeline::DATA_DSP_STREAM}; }
        std::vector<satdump::pipeline::ModuleDataType> getOutputTypes() override { return {satdump::pipeline::DATA_FILE, satdump::pipeline::DATA_STREAM}; }

        void init() override
        {
            if (d_preserve_input && (d_out_factor != 1 || input_data_type == satdump::pipeline::DATA_DSP_STREAM))
                throw satdump_exception("preserve_input needs byte input and out_factor 1");
            if (d_fail_init)
                throw satdump_exception("init failure on purpose");
            if (d_fail_running)
            {
                // Fork: wait for the module task, including its DSP block, before throwing on
                // the command thread. A loaded test machine may schedule that task late.
                satdump::eventBus->register_handler<LiveRunningEvent>([ready = d_running_ready](LiveRunningEvent)
                {
                    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(20);
                    while (!ready->load() && std::chrono::steady_clock::now() < end)
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    if (!ready->load())
                        throw satdump_exception("fault module did not start in time");
                    throw satdump_exception("running failure on purpose");
                });
            }
            if (d_with_block || d_block_fails)
            {
                d_block = std::make_shared<FaultBlock>();
                d_block->fail = d_block_fails;
            }
        }

        // Stops the block, as a demodulator's stop() does
        void stop() override
        {
            // Fork: stop also releases a deliberately stalled reader, including an early stop.
            {
                std::lock_guard<std::mutex> lock(d_stop_mutex);
                d_stopped = true;
            }
            d_stop_cv.notify_all();
            if (d_block)
                d_block->stop();
            // Fork: proves stop() ran, rather than merely reaching the process exit code.
            if (d_fail_running && d_running_ready->exchange(false))
                logger->info("Fault test running module stopped");
        }

        void process() override
        {
            Running running;

            if (d_block) // Like a demodulator, which starts its blocks here
                d_block->start();

            // Fork: publish only after the block has started; the event handler only reads
            // this atomic, never the block or the module's other state.
            if (d_fail_running)
                d_running_ready->store(true);

            if (d_hold_until_stop)
            {
                logger->info("Fault test consumer held until stop");
                std::unique_lock<std::mutex> lock(d_stop_mutex);
                d_stop_cv.wait(lock, [this] { return d_stopped; });
                logger->info("Fault test held consumer stopped");
                return;
            }

            // A block that fails on purpose has failed before this goes on. Its thread may first
            // run only after the module has stopped the block (a file input takes well under a
            // millisecond), and then work() never runs and nothing fails. 5 s at most
            if (d_block_fails)
            {
                auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (!d_block->has_failed() && std::chrono::steady_clock::now() < give_up)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                if (!d_block->has_failed())
                    logger->error("Fault test block did not fail within 5 s");
            }

            std::ifstream in;
            std::ofstream out;
            if (input_data_type == satdump::pipeline::DATA_FILE)
                in.open(d_input_file, std::ios::binary);
            if (output_data_type == satdump::pipeline::DATA_FILE)
            {
                d_output_file = d_output_file_hint + ".out";
                out.open(d_output_file, std::ios::binary);
            }

            std::vector<uint8_t> buf(4096);
            std::vector<uint8_t> obuf;
            int chunks = 0;
            while (true)
            {
                if (d_fail_after >= 0 && chunks >= d_fail_after)
                    fail();

                int got = 0;
                if (input_data_type == satdump::pipeline::DATA_FILE)
                {
                    in.read((char *)buf.data(), buf.size());
                    got = in.gcount();
                    if (got <= 0)
                        break;
                }
                else if (input_data_type == satdump::pipeline::DATA_STREAM)
                {
                    got = input_fifo->read(buf.data(), buf.size());
                    if (got <= 0) // The buffer was stopped
                        break;
                }
                else
                {
                    got = input_stream->read();
                    input_stream->flush();
                    if (got < 0) // The stream was stopped
                        break;
                    if (got == 0)
                        continue;
                }

                chunks++;
                bytes_in += got;

                if (d_preserve_input)
                    obuf.assign(buf.begin(), buf.begin() + got);
                else
                    obuf.assign((size_t)got * d_out_factor, (uint8_t)chunks);
                if (output_data_type == satdump::pipeline::DATA_FILE)
                {
                    out.write((char *)obuf.data(), obuf.size());
                    // Fork: let the test observe complete writes before requesting a stop.
                    if (d_preserve_input)
                        out.flush();
                }
                else
                    output_fifo->write(obuf.data(), obuf.size());
                bytes_out += obuf.size();
            }

            // Like a demodulator reading a file, which stops its blocks itself at the end
            if (input_data_type == satdump::pipeline::DATA_FILE)
                stop();
        }

        void drawUI(bool) override {}

        nlohmann::json getModuleStats() override
        {
            nlohmann::json v;
            v["bytes_in"] = (uint64_t)bytes_in;
            v["bytes_out"] = (uint64_t)bytes_out;
            return v;
        }

        std::string getIDM() override { return d_id; }
    };

    // Registers fork_fault_a and fork_fault_b (the same module under two names)
    inline void register_modules(std::vector<satdump::pipeline::ModuleEntry> &registry)
    {
        for (std::string id : {"fork_fault_a", "fork_fault_b"})
            registry.push_back(satdump::pipeline::ModuleEntry{
                id, nlohmann::json::object(), [id](std::string input_file, std::string output_file_hint, nlohmann::json parameters)
                { return std::shared_ptr<satdump::pipeline::ProcessingModule>(std::make_shared<FaultModule>(id, input_file, output_file_hint, parameters)); }});
    }
} // namespace fork_test
