#pragma once

#include "common/dsp/block_failures.h"
#include "common/dsp/buffer.h"
#include "complex.h"
#include "core/exception.h"
#include "logger.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <typeinfo>
#if defined(__GNUG__)
#include <cxxabi.h>
#endif

#define BRANCHLESS_CLIP(x, clip) (0.5 * (std::abs(x + clip) - std::abs(x - clip)))
#ifndef M_PI
#define M_PI 3.14159265358979323846 /* pi */
#endif

namespace dsp
{
    template <typename IN_T, typename OUT_T>
    class Block
    {
    protected:
        std::thread d_thread;
        // Fork: written by stop() and read by the block's thread. It was a plain bool (a data
        // race); it is now atomic.
        std::atomic<bool> should_run{false};
        virtual void work() = 0;
        bool d_got_input;
        void run()
        {
            // Fork: an exception thrown by work() left this thread and ended SatDump through
            // std::terminate. It is now caught here and logged, and the block winds down
            // without leaving the blocks around it waiting (see workFailed()).
            std::string error;
            try
            {
                while (should_run)
                {
                    work();

                    // Fork: the block before this one failed and will send nothing more. Pass
                    // that on to this block's output and end, so the chain winds down to its
                    // last reader instead of leaving it waiting for data forever.
                    if (d_got_input && input_stream && input_stream->writerFailedAndEmpty())
                    {
                        logger->debug("DSP block %s ends: the block before it failed", blockName().c_str());
                        if (output_stream)
                            output_stream->setWriterFailed();
                        return;
                    }
                }
                return;
            }
            catch (const std::exception &e)
            {
                error = e.what();
            }
            catch (...)
            {
                error = "unknown exception";
            }
            workFailed(error);
        }

        // Fork: extra workers belong to the same lifecycle as d_thread. Hooks run under
        // d_start_stop_mtx; start_extra() runs once per start, before the main worker.
        // stop_extra() must tolerate a partial start or no start, and wake and join its workers.
        virtual void start_extra() {}
        virtual void stop_extra() noexcept {}

    private:
        // Fork: held across the whole of start() and of stop(), so they run one at a time. A
        // stop() on one thread could clear should_run, then a start() on another thread (a live
        // module's task starting its blocks while a SIGTERM stops the run) set it again and
        // made a new thread, which stop() then joined: that thread never ends, as its input was
        // just stopped, so SatDump hung. The thread object was also written and joined at once
        // (a data race). Neither start() nor stop() calls the other or itself while holding it,
        // and the block's own thread never takes it.
        std::mutex d_start_stop_mtx;
        // Fork: a friend gives tests non-virtual access to the lifecycle mutex.
        friend struct BlockLifecycleTestAccess;

        std::atomic<bool> d_failed{false};
        std::mutex d_failure_mtx;
        std::string d_failure;

        // Fork: called on the block's thread when work() threw. Logs and records the error
        // (has_failed(), and the process-wide count in block_failures.h that makes a command
        // fail), tells the reader of the output that no more data will come, and keeps taking
        // (and dropping) the input until stop(), so the writer before this block is not left
        // waiting to hand over its next buffer.
        void workFailed(const std::string &error)
        {
            const std::string name = blockName();
            logger->error("DSP block %s failed: %s", name.c_str(), error.c_str());
            {
                std::lock_guard<std::mutex> lck(d_failure_mtx);
                d_failure = error;
            }
            d_failed = true;
            record_block_failure(name, error);

            if (output_stream)
                output_stream->setWriterFailed();

            if (d_got_input && input_stream)
            {
                while (should_run)
                {
                    if (input_stream->read() >= 0)
                        input_stream->flush();
                    else if (input_stream->writerFailedAndEmpty())
                        break; // The block before failed as well, nothing more will come
                    else
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
        }

        // Fork: the block's class name for log lines (legacy blocks have no ID)
        std::string blockName()
        {
            const char *raw = typeid(*this).name();
#if defined(__GNUG__)
            int status = 0;
            char *demangled = abi::__cxa_demangle(raw, nullptr, nullptr, &status);
            std::string name = (status == 0 && demangled != nullptr) ? demangled : raw;
            std::free(demangled);
            return name;
#else
            return raw;
#endif
        }

    protected:
        // Fork: extra-output blocks use the same start/stop lock while waking all writers and
        // joining. A template keeps this allocation-free; extra workers stop through stop_extra().
        template <typename StopOutputs>
        void stop_with_outputs(StopOutputs stop_outputs)
        {
            std::lock_guard<std::mutex> start_stop(d_start_stop_mtx);
            should_run = false;
            if (d_got_input && input_stream)
                input_stream->stopReader();
            stop_outputs();
            // Fork: wake the extra worker's buffers before joining the main worker,
            // which may be waiting to write to them.
            stop_extra();
            if (d_thread.joinable())
                d_thread.join();
        }

    public:
        std::shared_ptr<dsp::stream<IN_T>> input_stream;
        std::shared_ptr<dsp::stream<OUT_T>> output_stream;

    public:
        Block(std::shared_ptr<dsp::stream<IN_T>> input) : input_stream(input)
        {
            d_got_input = true;
            output_stream = std::make_shared<dsp::stream<OUT_T>>();
        }
        ~Block()
        {
            // Fork: also when the thread ended by itself (should_run already false): a
            // std::thread destroyed before join() calls std::terminate. Unlike the new DSP
            // Block this one can be stopped here: stop() wakes its own waits on both streams,
            // so the join returns, as it has done upstream for years. (The derived block is
            // already destroyed, so a thread caught between two work() calls can still stop
            // with "pure virtual method called". Owners must call stop() first.)
            if (should_run || d_thread.joinable())
            {
                logger->critical("CRITICAL! BLOCK SHOULD BE STOPPED BEFORE CALLING DESTRUCTOR!");
                stop();
            }
        }
        virtual void start()
        {
            std::lock_guard<std::mutex> start_stop(d_start_stop_mtx); // Fork: see d_start_stop_mtx

            // Fork: reject a duplicate start before either worker can be replaced.
            if (d_thread.joinable())
                throw satdump_exception("Block wasn't properly stopped before start() was called again!");

            // Fork: a block started again begins without a failure, with its output open
            d_failed = false;
            {
                std::lock_guard<std::mutex> lck(d_failure_mtx);
                d_failure.clear();
            }
            if (output_stream)
                output_stream->clearWriterFailed();
            should_run = true;
            try
            {
                start_extra();
                d_thread = std::thread(&Block::run, this);
            }
            catch (...)
            {
                // Fork: roll back all workers if creating either thread failed.
                should_run = false;
                stop_extra();
                throw;
            }
        }
        virtual void stop()
        {
            stop_with_outputs([&]()
            {
                if (output_stream)
                    output_stream->stopWriter();
            });
        }

        // Fork: true if work() failed (threw) since the last start(); the error was logged
        bool has_failed() { return d_failed; }

        // Fork: what work() threw, empty if it did not fail
        std::string failure_message()
        {
            std::lock_guard<std::mutex> lck(d_failure_mtx);
            return d_failure;
        }
    };

    template <typename IN_T, typename OUT_T>
    class HierBlock
    {
    public:
        std::shared_ptr<dsp::stream<IN_T>> input_stream;
        std::shared_ptr<dsp::stream<OUT_T>> output_stream;

    public:
        HierBlock(std::shared_ptr<dsp::stream<IN_T>> input) : input_stream(input) {}
        ~HierBlock() {}
        virtual void start() = 0;
        virtual void stop() = 0;
    };

    // This is here as many blocks require it
    float branchless_clip(float x, float clip);
    float branched_clip(float x, float clip);
    double hz_to_rad(double freq, double samplerate);
    double rad_to_hz(double rad, double samplerate);
} // namespace dsp