#pragma once

#include "common/dsp/block.h"
#include "common/dsp/buffer.h"
#include "common/dsp/complex.h"
#include "common/dsp/filter/firdes.h"
#include "dsp/block.h"
#include <atomic>
#include <volk/volk.h>
#include <volk/volk_alloc.hh>

namespace satdump
{
    namespace ndsp
    {
        template <typename T>
        class ReshapeBufferBlock : public Block
        {
        private:
            int buffer_size = 1024;
            dsp::RingBuffer<T> ring_buf{100};

            // Fork: written by stop() and read by the work2 thread, so atomic
            std::atomic<bool> work2shouldrun{false};
            std::thread work2Thread;

            bool work();
            void work2();

        public:
            ReshapeBufferBlock();
            ~ReshapeBufferBlock();

        protected:
            // Fork: the base serialises these with its own worker's start and join.
            void start_extra() override;
            void stop_extra() noexcept override;

        public:

            void init() { ring_buf.reset(); }

            nlohmann::ordered_json get_cfg_list()
            {
                nlohmann::ordered_json p;
                add_param_simple(p, "buffer_size", "int");
                return p;
            }

            nlohmann::json get_cfg(std::string key)
            {
                if (key == "buffer_size")
                    return buffer_size;
                else
                    throw satdump_exception(key);
            }

            cfg_res_t set_cfg(std::string key, nlohmann::json v)
            {
                if (key == "buffer_size")
                    buffer_size = v;
                else
                    throw satdump_exception(key);
                return RES_OK;
            }
        };
    } // namespace ndsp
} // namespace satdump
