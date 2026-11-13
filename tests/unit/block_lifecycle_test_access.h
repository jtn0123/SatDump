#pragma once

// Fork: inspect lifecycle ownership from a different thread without changing either vtable.
#include "common/dsp/block.h"
#include "dsp/block.h"

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
        struct BlockLifecycleTestAccess
        {
            static std::mutex &mutex(Block &block) { return block.blk_th_mtx; }
            static bool main_done(Block &block) { return block.blk_th_done; }
        };
    }
}
