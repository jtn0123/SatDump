#include "core/exception.h"
#include "dsp/block.h"
#include "dsp/block_start.h"
#include "dsp/path/splitter.h"
#include "flowgraph.h"
#include "logger.h"
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace satdump
{
    namespace ndsp
    {
        namespace flowgraph
        {
            namespace
            {
                // Fork: the blocks in start order for start_blocks_or_undo(): each block after
                // all the blocks that read its outputs, so sources start last. Blocks in a loop
                // (there should be none) keep their order, at the end.
                std::vector<Block *> readersFirst(const std::vector<std::shared_ptr<Block>> &blocks)
                {
                    const size_t n = blocks.size();
                    std::vector<std::vector<size_t>> readers(n);
                    for (size_t w = 0; w < n; w++)
                        for (auto &o : blocks[w]->get_outputs())
                            if (o.fifo)
                                for (size_t r = 0; r < n; r++)
                                    for (auto &i : blocks[r]->get_inputs())
                                        if (r != w && i.fifo == o.fifo)
                                            readers[w].push_back(r);

                    std::vector<Block *> order;
                    std::vector<bool> placed(n, false);
                    bool progress = true;
                    while (order.size() < n && progress)
                    {
                        progress = false;
                        for (size_t b = 0; b < n; b++)
                        {
                            if (placed[b])
                                continue;
                            bool ready = true;
                            for (size_t r : readers[b])
                                if (!placed[r])
                                    ready = false;
                            if (ready)
                            {
                                placed[b] = true;
                                order.push_back(blocks[b].get());
                                progress = true;
                            }
                        }
                    }
                    for (size_t b = 0; b < n; b++)
                        if (!placed[b])
                            order.push_back(blocks[b].get());
                    return order;
                }
            } // namespace

            bool Flowgraph::run()
            {
                is_running = true;
                bool ok = true;

                try
                {
                    // Update variables
                    updateVars();

                    // Hold all input IDs that will be connected
                    std::vector<int> all_connected_inputs;

                    // Holds stuff such as splitters when one output goes to more than
                    // one input
                    std::vector<std::shared_ptr<ndsp::Block>> additional_blocks;

                    // Iterate through all nodes
                    for (auto &n : nodes)
                    {
                        if (n->disabled)
                            continue;

                        if (n->internal->is_errored())
                            throw satdump_exception("A node has an error. Abort.");

                        // UI Stuff, safety
                        n->show_vars_win = false;

                        auto &blk = n->internal->blk;

                        // Iterate through outputs
                        for (int o = 0; o < blk->get_outputs().size(); o++)
                        {
                            // Get output ID
                            int o_id = -1;
                            for (int c = 0, oc = 0; c < n->node_io.size(); c++)
                            {
                                if (n->node_io[c].is_out)
                                {
                                    if (oc == o)
                                        o_id = n->node_io[c].id;
                                    oc++;
                                }
                            }

                            // Iterate through links, to asign outputs to applicable inputs
                            struct InputH
                            {
                                std::shared_ptr<ndsp::Block> blk;
                                int idx;
                            };
                            std::vector<InputH> inputs_to_feed;
                            for (auto &l : links)
                            {
                                if (l.start == o_id)
                                {
                                    // Iterate through nodes to find valid inputs
                                    for (auto &n2 : nodes)
                                    {
                                        if (n2->disabled)
                                            continue;

                                        for (int b = 0, b2 = 0; b < n2->node_io.size(); b++)
                                        {
                                            if (!n2->node_io[b].is_out)
                                            {
                                                if (n2->node_io[b].id == l.end)
                                                {
                                                    inputs_to_feed.push_back({n2->internal->blk, b2});
                                                    // logger->trace("Assigned to : " + n2->internal->blk->d_id);
                                                    all_connected_inputs.push_back(l.end);
                                                }

                                                b2++;
                                            }
                                        }
                                    }
                                }

                                if (l.end == o_id)
                                {
                                    // Iterate through nodes to find valid inputs
                                    for (auto &n2 : nodes)
                                    {
                                        if (n2->disabled)
                                            continue;

                                        for (int b = 0, b2 = 0; b < n2->node_io.size(); b++)
                                        {
                                            if (!n2->node_io[b].is_out)
                                            {
                                                if (n2->node_io[b].id == l.start)
                                                {
                                                    inputs_to_feed.push_back({n2->internal->blk, b2});
                                                    // logger->trace("Assigned to : " + n2->internal->blk->d_id);
                                                    all_connected_inputs.push_back(l.start);
                                                }

                                                b2++;
                                            }
                                        }
                                    }
                                }
                            }

                            // If needed, add splitter
                            if (inputs_to_feed.size() == 1)
                            {
                                inputs_to_feed[0].blk->link(blk.get(), o, inputs_to_feed[0].idx, 16 /*TODOREWORK*/);
                            }
                            else if (inputs_to_feed.size() > 1)
                            {
                                // logger->error("More than one to connect! Adding splitter");

                                std::shared_ptr<ndsp::Block> ptr;
                                if (blk->get_output(o, 0).type == ndsp::BlockIOType::DSP_SAMPLE_TYPE_CF32)
                                    ptr = std::make_shared<ndsp::SplitterBlock<complex_t>>();
                                else if (blk->get_output(o, 0).type == ndsp::BlockIOType::DSP_SAMPLE_TYPE_F32)
                                    ptr = std::make_shared<ndsp::SplitterBlock<float>>();
                                else if (blk->get_output(o, 0).type == ndsp::BlockIOType::DSP_SAMPLE_TYPE_S16)
                                    ptr = std::make_shared<ndsp::SplitterBlock<int16_t>>();
                                else if (blk->get_output(o, 0).type == ndsp::BlockIOType::DSP_SAMPLE_TYPE_S8)
                                    ptr = std::make_shared<ndsp::SplitterBlock<int8_t>>();
                                else if (blk->get_output(o, 0).type == ndsp::BlockIOType::DSP_SAMPLE_TYPE_U8)
                                    ptr = std::make_shared<ndsp::SplitterBlock<uint8_t>>();
                                else
                                    throw satdump_exception("Unsupported splitter block IO type");

                                nlohmann::json p;
                                p["noutputs"] = inputs_to_feed.size();
                                ptr->set_cfg(p);

                                ptr->link(blk.get(), o, 0, 16 /*TODOREWORK*/);
                                for (int v = 0; v < inputs_to_feed.size(); v++)
                                    inputs_to_feed[v].blk->link(ptr.get(), v, inputs_to_feed[v].idx, 16 /*TODOREWORK*/);

                                additional_blocks.push_back(ptr);
                            }
                            else if (inputs_to_feed.size() == 0)
                            {
                                throw satdump_exception("Block has unconnected output!");
                            }
                        }
                    }

                    // Iterate through all nodes, check all inputs are connected
                    for (auto &n : nodes)
                    {
                        if (n->disabled)
                            continue;

                        for (auto &i : n->node_io)
                        {
                            if (i.is_out)
                                continue;

                            bool missing = true;
                            for (auto &c : all_connected_inputs)
                                if (i.id == c)
                                    missing = false;

                            if (missing)
                                throw satdump_exception("Block (" + n->title + ") has unconnected input!");
                        }
                    }

                    // Start them all.
                    // Fork: readers first, and when one fails to start, the ones already
                    // started are stopped again before the error is reported. They were left
                    // running, and destroying them later stopped SatDump.
                    std::vector<std::shared_ptr<ndsp::Block>> all_blocks;
                    for (auto &n : nodes)
                        if (!n->disabled)
                            all_blocks.push_back(n->internal->blk);
                    for (auto &b : additional_blocks)
                        all_blocks.push_back(b);
                    start_blocks_or_undo(readersFirst(all_blocks));
                    for (auto &n : nodes)
                        if (!n->disabled)
                            n->internal->upd_state();

                    // And then wait for them to exit
                    for (auto &n : nodes)
                        if (!n->disabled && !n->internal->blk->is_async())
                            n->internal->blk->stop();
                    for (auto &b : additional_blocks)
                        b->stop();

                    // Restop them all, including async ones
                    for (auto &n : nodes)
                        if (!n->disabled)
                            n->internal->blk->stop();

                    // Re-update UI
                    for (auto &n : nodes)
                        if (!n->disabled)
                            n->internal->upd_state();

                    // Fork: a block whose work() failed has logged why, ended its outputs and
                    // stopped, so the flowgraph got here the normal way. Say that it failed.
                    for (auto &b : all_blocks)
                    {
                        if (b->has_failed())
                        {
                            logger->error("Flowgraph block %s failed: %s", b->d_id.c_str(), b->get_failure().c_str());
                            ok = false;
                        }
                    }
                }
                catch (std::exception &e)
                {
                    logger->error("Error running flowgraph : %s", e.what());
                    ok = false;
                }
                catch (...)
                {
                    // Fork: this runs on the UI's flowgraph thread, so anything else thrown
                    // (a block's start() can throw anything) ended SatDump (std::terminate)
                    logger->error("Error running flowgraph : unknown exception");
                    ok = false;
                }

                is_running = false;
                return ok;
            }

            void Flowgraph::stop()
            {
                try
                {
                    std::vector<std::thread> all_th;

                    // Iterate through all nodes
                    for (auto &n : nodes)
                    {
                        if (n->disabled)
                            continue;

                        // Stop only those that are sources
                        if (n->internal->blk->is_async())
                        {
                            auto v = [&]
                            {
                                // Fork: an exception from stop() left this thread, which is
                                // std::terminate. It is now logged and the other sources still stop.
                                const std::string id = n->internal->blk->d_id;
                                try
                                {
                                    logger->trace("Stopping source " + id);
                                    n->internal->blk->stop(true);
                                    logger->trace("Stopped source " + id);
                                }
                                catch (const std::exception &e)
                                {
                                    logger->error("Error stopping flowgraph block %s : %s", id.c_str(), e.what());
                                }
                                catch (...)
                                {
                                    logger->error("Error stopping flowgraph block %s : unknown exception", id.c_str());
                                }
                            };
                            all_th.push_back(std::thread(v));
                        }
                    }

                    // Wait
                    for (auto &v : all_th)
                        if (v.joinable())
                            v.join();
                }
                catch (std::exception &e)
                {
                    logger->error("Error running flowgraph : %s", e.what());
                }
            }
        } // namespace flowgraph
    } // namespace ndsp
} // namespace satdump