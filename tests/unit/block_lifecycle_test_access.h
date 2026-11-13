#pragma once

// Fork: inspect lifecycle ownership from a different thread without changing either vtable.
#include "common/dsp/block.h"
#include "dsp/block.h"
#include "dsp/utils/reshape_buffer.h"

namespace dsp
{
    struct BlockLifecycleTestAccess
    {
        template <typename I, typename O> static std::mutex &mutex(Block<I, O> &block)
        {
            return block.d_start_stop_mtx;
        }
    };
}
namespace satdump
{
    namespace ndsp
    {
        // Fork: all access except readable() occurs before start or after the workers join.
        struct ReshapeBufferTestAccess
        {
            template <typename T> static void force_size(ReshapeBufferBlock<T> &block, int size)
            {
                block.buffer_size = size;
            }
            template <typename T> static bool extra_joinable(ReshapeBufferBlock<T> &block)
            {
                return block.work2Thread.joinable();
            }
            template <typename T> static int readable(ReshapeBufferBlock<T> &block)
            {
                return block.ring_buf.getReadable();
            }
            template <typename T> static size_t staging_size(ReshapeBufferBlock<T> &block)
            {
                return block.output_chunk.size();
            }
        };
        struct BlockLifecycleTestAccess
        {
            static std::mutex &mutex(Block &block) { return block.blk_th_mtx; }
            static bool main_done(Block &block) { return block.blk_th_done; }
            static bool main_joinable(Block &block) { return block.blk_th.joinable(); }
        };
    }
}
