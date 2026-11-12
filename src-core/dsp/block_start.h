#pragma once

/**
 * @file block_start.h
 * @brief Fork: start a group of blocks without leaving some of them running when one fails
 */

#include "dsp/block.h"
#include "logger.h"
#include <set>
#include <vector>

namespace satdump
{
    namespace ndsp
    {
        /**
         * @brief Stop blocks that were started as part of a group whose start failed.
         *
         * They must have been started readers first (see start_blocks_or_undo()), so every
         * block that reads their outputs is among them. Sources among them may already have
         * sent data. Each of their inputs that none of them writes has no running writer (it
         * never started), so it gets a terminator here; then they are stopped writers first:
         * each one ends on a terminator, or on stop_now for a source, and passes it on. A block
         * with several inputs that ended on one of them keeps taking the others until their
         * writers end (see Block::end_streams()), so no writer is left waiting for room.
         * @param started the blocks that did start, in the order they were started
         */
        inline void stop_started_blocks(const std::vector<Block *> &started)
        {
            std::set<DSPStream *> written;
            for (auto *b : started)
                for (auto &o : b->get_outputs())
                    if (o.fifo)
                        written.insert(o.fifo.get());

            for (auto *b : started)
                for (auto &i : b->get_inputs())
                    if (i.fifo && written.count(i.fifo.get()) == 0)
                        if (!i.fifo->try_send_terminator())
                            logger->warn("Could not end the input of DSP block %s", b->d_id.c_str());

            for (auto it = started.rbegin(); it != started.rend(); it++)
                (*it)->stop(true);
        }

        /**
         * @brief Start blocks in the order given. If one fails to start, the ones
         * already started are stopped again (stop_started_blocks()) and the error is passed
         * on. Left running they would never end, and destroying them stops SatDump
         * (see ~Block).
         *
         * The order must put every block after the blocks that read its outputs (readers
         * first, sources last).
         * @param blocks the blocks to start
         */
        inline void start_blocks_or_undo(const std::vector<Block *> &blocks)
        {
            std::vector<Block *> started;
            try
            {
                for (auto *b : blocks)
                {
                    b->start();
                    started.push_back(b);
                }
            }
            catch (...)
            {
                stop_started_blocks(started);
                throw;
            }
        }
    } // namespace ndsp
} // namespace satdump
