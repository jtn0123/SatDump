#pragma once

/**
 * @file block.h
 * @brief Base DSP block class, more cumbersome to use. Prefer wrappers if possible
 */

#include "base/dsp_buffer.h"
#include "common/dsp/block_failures.h"
#include "core/exception.h"
#include "dsp/base/stream.h"
#include "logger.h"
#include "nlohmann/json.hpp"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace satdump
{
    namespace ndsp
    {
        /**
         * @brief Block IO types.
         *
         * This is helpful to enforce & check type compatibility
         * whenever that may be needed
         */
        enum BlockIOType
        {
            DSP_SAMPLE_TYPE_CF32,
            DSP_SAMPLE_TYPE_F32,
            DSP_SAMPLE_TYPE_S16,
            DSP_SAMPLE_TYPE_S8,
            DSP_SAMPLE_TYPE_U8,
        };

        /**
         * @brief Block IO helper class
         *
         * @param name of the input/output. Mostly meant for when blocks
         * are rendered in a flowchart
         * @param type of the data/samples expected on this IO
         * @param fifo the actual fifo to connect to other blocks
         * @param blkdata optional data the blocks can chose to hold in IQ
         */
        struct BlockIO
        {
            std::string name;
            BlockIOType type;
            std::shared_ptr<DSPStream> fifo = nullptr;
            std::shared_ptr<void> blkdata = nullptr;

            // TODOREWORK DOCUMENT
            uint64_t samplerate = 0;
            double frequency = 0;
        };

        // TODOREWORK cleanup!!!!
        inline void add_param_simple(nlohmann::ordered_json &p, std::string id, std::string type, std::string name = "")
        {
            p[id]["type"] = type;
            if (name != "")
                p[id]["name"] = name;
        }

        inline void add_param_range(nlohmann::ordered_json &p, std::string id, std::string type, nlohmann::ordered_json min, nlohmann::ordered_json max, nlohmann::ordered_json step,
                                    std::string name = "")
        {
            p[id]["type"] = type;
            if (name != "")
                p[id]["name"] = name;
            p[id]["range"] = {min, max, step};
        }

        inline void add_param_list(nlohmann::ordered_json &p, std::string id, std::string type, nlohmann::json list, std::string name = "")
        {
            p[id]["type"] = type;
            if (name != "")
                p[id]["name"] = name;
            p[id]["list"] = list;
        }

        /**
         * @brief Base Block class.
         *
         * This has several goals :
         *
         * - First this simply provides an easy-to-use base to implement most
         * DSP blocks, with inputs/outputs passing pointers around, as well
         * as an internal thread to run the actual loop,
         *
         * - Secondly, unlike the previous system this class' interface alone
         * is meant to be enough to perform ALL block functions, from
         * configurations to starting/stopping and so on, as well as entirely
         * abstracting types used in IO streams.
         *
         * See each function's documentation for more information!
         *
         * @param d_id the block's ID
         * @param inputs the block's inputs
         * @param outputs the block's outputs
         */
        class Block
        {
        public:
            const std::string d_id;

        protected:
            std::vector<BlockIO> inputs;
            std::vector<BlockIO> outputs;

        public:
            /**
             * @brief Get the block's input configurations and
             * streams. You should not modify them.
             * @return The block's input BlockIOs.
             */
            virtual std::vector<BlockIO> get_inputs() { return inputs; }

            /**
             * @brief Get the block's output configurations and
             * streams. You should not modify them.
             * @return The block's output BlockIOs.
             */
            virtual std::vector<BlockIO> get_outputs() { return outputs; }

            /**
             * @brief Link an input to an output stream of some sort.
             * This also checks the type of the BlockIO to ensure
             * (some level of) compatibility.
             * @param f input BlockIO to link to an input
             * @param i index of the input (probably quite often 0)
             */
            virtual void set_input(BlockIO f, int i)
            {
                if (i >= (int)inputs.size())
                    throw satdump_exception("Input index " + std::to_string(i) + " does not exist for " + d_id + "!");
                if (inputs[i].type != f.type)
                    throw satdump_exception("Input type " + std::to_string(inputs[i].type) + " not compatible with " + std::to_string(f.type) + " for " + d_id + "!");
                inputs[i].fifo = f.fifo;
            }

            /**
             * @brief Get one of the block's outputs, creating
             * the fifo it if nbuf != 0
             * @param i index of the output
             * @param nbuf of buffers to setup in the output
             * @return BlockIO struct of the output
             */
            virtual BlockIO get_output(int i, int nbuf)
            {
                if (i >= (int)outputs.size())
                    throw satdump_exception("Ouput index " + std::to_string(i) + " does not exist for " + d_id + "!");
                if (nbuf > 0)
                    outputs[i].fifo = std::make_shared<DSPStream>(nbuf);
                return outputs[i];
            }

            /**
             * @brief Link a block's output to another input,
             * more or less just a warper around set_input and
             * set_output
             * @param ptr block to get the output from
             * @param output_index index of the output to use,
             * from ptr
             * @param input_index index of the input to asign no
             * (this block, NOT ptr)
             * @param nbuf of buffers to setup in the output (see
             * set_input for more info)
             * @return BlockIO struct of the output
             */
            void link(Block *ptr, int output_index, int input_index, int nbuf) { set_input(ptr->get_output(output_index, nbuf), input_index); }

        private:
            // Fork: blk_should_run and work_should_exit are written by stop() and read by the
            // block's thread. They were plain bools (a data race); they are now atomic.
            std::atomic<bool> blk_should_run;
            std::mutex blk_th_mtx;
            std::thread blk_th;

            std::atomic<bool> blk_failed;
            std::mutex blk_failure_mtx;
            std::string blk_failure;

            // Fork: set by the block's thread as its last step, after which it no longer uses
            // this object (so ~Block can join it safely)
            std::atomic<bool> blk_th_done;

            void run()
            {
                run_work();
                blk_th_done = true;
            }

            void run_work()
            {
                // Fork: an exception thrown by work() left this thread and ended SatDump through
                // std::terminate. It is now caught here, logged with the block ID, and the block
                // winds down without leaving the blocks around it waiting (see work_failed()).
                std::string error;
                try
                {
                    while (blk_should_run)
                        if (work())
                            break;
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
                work_failed(error);
            }

            /**
             * @brief Called on the block's thread when work() threw. Logs the error and
             * records it (has_failed(), get_failure(), and the process-wide count in
             * common/dsp/block_failures.h that makes a command fail), then ends this block's
             * part of the flowgraph with end_streams(): the blocks after it end the way they do
             * at the end of a stream, and the blocks before it never wait for room.
             * The thread then ends. stop() works as usual; stop(..., true) cuts the wait short.
             * @param error what work() threw
             */
            void work_failed(const std::string &error)
            {
                logger->error("DSP block %s failed: %s", d_id.c_str(), error.c_str());
                {
                    std::lock_guard<std::mutex> lg(blk_failure_mtx);
                    blk_failure = error;
                }
                blk_failed = true;
                ::dsp::record_block_failure(d_id, error);

                std::vector<std::shared_ptr<DSPStream>> outs, ins;
                for (auto &o : outputs)
                    if (o.fifo)
                        outs.push_back(o.fifo);
                for (auto &i : inputs)
                    if (i.fifo)
                        ins.push_back(i.fifo);
                // The block after it may have stopped too, so do not wait for it forever
                end_streams(outs, ins, true);
            }

        protected:
            /**
             * @brief Used to signal to a block it should exit in order
             * to stop(). Usually only needed in sources.
             */
            std::atomic<bool> work_should_exit;

            /**
             * @brief The actual looping work function meant to handle
             * all the DSP (well, in most blocks)
             *
             * @return true if block's thread should exit, usually upon
             * receiving a terminator in the input stream
             */
            virtual bool work() = 0;

            /**
             * @brief Check if the work function is running.
             * @return true if running
             */
            bool is_work_running() { return blk_should_run; }

            /**
             * @brief Fork: end this block's part of the flowgraph without waiting on a
             * neighbour that may never move again. A terminator is sent on each of `outs` as
             * soon as its reader frees a buffer, and whatever arrives on each of `ins` is taken
             * and dropped until its terminator arrives, so the writers before this block never
             * wait for room. Both happen in one loop, as either can be what the other waits for.
             *
             * Used by work_failed(), and by blocks with several inputs once one input ended
             * (see BlockSimpleMulti): before, such a block kept the buffers it had taken and
             * left its other inputs full, so the blocks writing them waited forever.
             * Returns early on stop(..., true).
             * @param outs streams to end
             * @param ins streams to drain until their terminator
             * @param may_give_up if true, stop trying to end `outs` 2 s after `ins` ended (the
             * reader may have stopped: a failed block must not hang stop()); if false, wait as
             * long as it takes, like wait_enqueue() (normal end of stream)
             */
            void end_streams(std::vector<std::shared_ptr<DSPStream>> outs, std::vector<std::shared_ptr<DSPStream>> ins, bool may_give_up)
            {
                const auto patience = std::chrono::seconds(2);
                auto give_up = std::chrono::steady_clock::now() + patience;
                while (!outs.empty() || !ins.empty())
                {
                    bool busy = false;
                    for (size_t o = 0; o < outs.size();)
                    {
                        if (outs[o]->try_send_terminator())
                        {
                            outs.erase(outs.begin() + o);
                            busy = true;
                        }
                        else
                            o++;
                    }
                    for (size_t i = 0; i < ins.size();)
                    {
                        DSPBuffer b;
                        bool ended = false;
                        while (!ended && ins[i]->try_dequeue(b))
                        {
                            ended = b.isTerminator();
                            ins[i]->free(b);
                            busy = true;
                        }
                        if (ended)
                            ins.erase(ins.begin() + i);
                        else
                            i++;
                    }

                    if (!blk_should_run) // stop(..., true)
                        break;
                    auto now = std::chrono::steady_clock::now();
                    if (!ins.empty())
                        give_up = now + patience;
                    else if (may_give_up && !outs.empty() && now > give_up)
                    {
                        logger->warn("DSP block %s: the block after it takes no more buffers, its end of stream was not passed on", d_id.c_str());
                        break;
                    }
                    if (!busy)
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }

        public:
            /**
             * @brief Check if work() failed (threw) since the last start().
             * The block has then logged the error and ended its outputs. Its thread
             * keeps taking and dropping its inputs until their writers end (see
             * work_failed()), so stop() must still be called.
             * @return true if work() failed
             */
            bool has_failed() { return blk_failed; }

            /**
             * @brief Check if the block's thread has ended by itself since the last start()
             * (end of its stream, or work() failed). stop() then returns at once.
             * @return true if the thread has ended
             */
            bool has_ended() { return blk_th_done; }

            /**
             * @brief Get what work() threw, if it failed (see has_failed())
             * @return the error message, empty if it did not fail
             */
            std::string get_failure()
            {
                std::lock_guard<std::mutex> lg(blk_failure_mtx);
                return blk_failure;
            }

        public:
            /**
             * @brief Generic constructor, to be overloaded.
             * @param id of the block, meant to be a short identifier (eg, agc_cc)
             * @param in Inputs configurations, optional
             * @param out Output configurations, optional
             */
            Block(std::string id, std::vector<BlockIO> in = {}, std::vector<BlockIO> out = {}) : d_id(id), inputs(in), outputs(out)
            {
                blk_should_run = false;
                work_should_exit = false;
                blk_failed = false;
                blk_th_done = false;
            }

            virtual ~Block()
            {
                // Fork: this threw, and a throw from a destructor always ends in std::terminate
                // (the -Wterminate warning). A block whose thread already ended by itself (end of
                // its stream, or work() failed) is now joined here, which is safe as that thread
                // no longer uses the block. Nothing safe can be done with a block that is still
                // running: the derived block is already destroyed, so its thread may call work()
                // of a destroyed object (C++ then stops with "pure virtual method called") or wait
                // forever on a stream (no join), and a std::thread that is destroyed or detached
                // while running is std::terminate or use after free. So that is still a stop, now
                // a deliberate one: the log names the block, and the abort gives the crash
                // handler's backtrace. Owners must call stop() first.
                if (blk_th.joinable() && blk_th_done)
                {
                    blk_th.join(); // Returns at once, the thread has ended
                    blk_should_run = false;
                }
                if (blk_should_run || blk_th.joinable())
                {
                    if (logger)
                        logger->critical("DSP block %s was destroyed while running, stop() was not called first. Stopping SatDump.", d_id.c_str());
                    std::abort();
                }
            }

        public:
            /**
             * @brief Returns true if a block is async.
             * An async block is a block that has outputs
             * without inputs or inputs that won't directly
             * affect/stop the outputs' behavior. This includes
             * sources (eg, a SDR Device), TXing and RXing at the
             * same time, and anything with similar requirements.
             * @return true if the block is async
             */
            virtual bool is_async() { return inputs.size() == 0; }

        public:
            /**
             * @brief set_cfg status.
             * @param RES_OK Applied fine
             * @param RES_LISTUPD get_cfg_list must be pulled again
             * @param RES_IOUPD IO Configuration changed (must ALSO re-pull get_cfg_list!!!)
             * @param RES_ERR Config option not applied
             */
            enum cfg_res_t
            {
                RES_OK = 0,
                RES_LISTUPD = 1,
                RES_IOUPD = 2,
                RES_ERR = 3,
            };

            /**
             * @brief Get parameters *LIST* of the block's
             * parameters. This does not contain actual values,
             * only a description of what is available. This
             * will contains a name, optionally description,
             * its type and range if applicable, string options
             * and so on.
             * This must be re-pulled in several cases :
             * - If set_cfg returns RES_LISTUPD or higher
             * - If the block is started
             * - If the block is stopped
             *
             * @return parameters description
             */
            virtual nlohmann::ordered_json get_cfg_list() { return {}; }

            /**
             * @brief Get parameters of the block as JSON
             * @param key the parameter to retrieve
             * @return requested parameter value
             */
            virtual nlohmann::json get_cfg(std::string key) = 0;

            /**
             * @brief Get parameters of the block as JSON.
             * Unlike get_cfg(key), this returns every single
             * available parameter as declared by get_cfg_list().
             * @return All parameters values
             */
            nlohmann::json get_cfg()
            {
                nlohmann::json p;
                auto v = get_cfg_list();
                for (auto &v2 : v.items())
                    p[v2.key()] = get_cfg(v2.key());
                return p;
            }

            /**
             * @brief Set parameters of the block from JSON,
             * including potentially IO configurations for blocks
             * that may have variable output sizes. However,
             * you likely should implemenet that in a separate
             * function as well (eg, addVFO or such) for it to be
             * easy to be done in C++ directly, and using said
             * function here.
             * Optionally, this can also be made to be functional
             * while the block is running!
             * @param key parameter to set
             * @param v value to set
             * @return error code or status.
             */
            virtual cfg_res_t set_cfg(std::string key, nlohmann::json v) = 0;

            /**
             * @brief Set parameters of the block from JSON.
             * Essentially the same as set_cfg(key, v), except
             * this will set any number of them at once.
             * @param v values to set
             * @return highest error code or status
             */
            cfg_res_t set_cfg(nlohmann::json v)
            {
                cfg_res_t r = RES_OK;
                for (auto &i : v.items())
                {
                    try
                    {
                        cfg_res_t r2 = set_cfg(i.key(), i.value());
                        if (r2 > r)
                            r = r2;
                    }
                    catch (const std::exception &e)
                    {
                        // Fork: the error was dropped without a word; it is now logged
                        logger->warn("DSP block %s: setting %s was not applied: %s", d_id.c_str(), i.key().c_str(), e.what());
                        r = RES_ERR;
                    }
                }
                return r;
            }

        protected:
            template <typename T>
            void setValFromJSONIfExists(T &v, nlohmann::json p)
            {
                if (!p.is_null())
                    v = p.get<T>();
            }

        protected:
            /**
             * @brief Applies current parameters to the block.
             * This is called automatically once in start(), but
             * may also be called manually by the block as needed.
             */
            virtual void init() {}

        public:
            /**
             * @brief Starts this block's internal thread and loop.
             */
            virtual void start()
            {
                if (blk_should_run || blk_th.joinable())
                    throw satdump_exception("Block wasn't properly stopped before start() was called again!");

                init();
                work_should_exit = false;
                blk_failed = false;
                {
                    std::lock_guard<std::mutex> lg(blk_failure_mtx);
                    blk_failure.clear();
                }
                blk_th_done = false;
                blk_should_run = true;
                blk_th = std::thread(&Block::run, this);
#ifndef _WIN32
#ifndef __APPLE__
                std::string th_id = d_id;
                th_id.resize(15);
                pthread_setname_np(blk_th.native_handle(), th_id.c_str());
#endif
#endif
            }

            /**
             * @brief Stops the block, or rather tells the internal
             * loop it should exit & joins the thread to wait.
             * TODOREWORK, potentially allow sending the terminator
             * as well to force-quit.
             * @param stop_now the normal stop just marks the block
             * for stopping when convenient, when the work() function
             * exists by itself. Setting this to true will tell the block
             * to stop now if it can send a terminator.
             * @param force this will exit work() as soon as
             * possible, leaving it in an unknown state. Do not
             * use until it is for live-reconfiguration!
             */
            virtual void stop(bool stop_now = false, bool force = false)
            { // TODOREWORK allow sending terminator in this function?
                if (stop_now)
                    work_should_exit = true;
                if (force)
                    blk_should_run = false;

                {
                    std::lock_guard<std::mutex> lg(blk_th_mtx);
                    if (blk_th.joinable())
                        blk_th.join();
                }
                blk_should_run = false;
            }
        };
    } // namespace ndsp
} // namespace satdump