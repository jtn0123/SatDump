#pragma once

#include "common/dsp/block.h"
#include "common/dsp/buffer.h"
#include "common/dsp/complex.h"
#include "common/dsp/filter/firdes.h"
#include "dsp/block.h"
#include <atomic>
#include <vector>
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
            // Fork: UI configuration and the output worker share the next chunk size.
            std::atomic<int> buffer_size{1024};
            static_assert(sizeof(std::atomic<int>) == sizeof(int) && alignof(std::atomic<int>) == alignof(int),
                          "Atomic chunk size must preserve the DSP plugin layout");
            friend struct ReshapeBufferTestAccess; // Fork: invalid-state and chunk-boundary tests.
            dsp::RingBuffer<T> ring_buf{100};

            // Fork: only the output worker produces output, including EOF. Stage a chunk
            // before allocating an output buffer so a discarded tail owns no stream buffer.
            std::vector<T> output_chunk;
            std::atomic<bool> propagate_eof{false};
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

            void init() override
            {
                // Fork: validate again before any worker can start, even for restored state.
                const int size = buffer_size.load();
                if (size <= 0)
                    throw satdump_exception("Reshape buffer size must be greater than zero");
                DSPStream::checked_sample_bytes(static_cast<size_t>(size), sizeof(T));
                ring_buf.reset();
                propagate_eof = false;
                output_chunk.resize(size);
            }

            nlohmann::ordered_json get_cfg_list()
            {
                nlohmann::ordered_json p;
                add_param_simple(p, "buffer_size", "int");
                return p;
            }

            nlohmann::json get_cfg(std::string key)
            {
                if (key == "buffer_size")
                    return buffer_size.load();
                else
                    throw satdump_exception(key);
            }

            cfg_res_t set_cfg(std::string key, nlohmann::json v)
            {
                if (key == "buffer_size")
                {
                    // Fork: reject invalid requests without changing the last valid size.
                    if (!v.is_number_integer())
                        return RES_ERR;
                    const int64_t size = v.get<int64_t>();
                    if (size <= 0 || static_cast<uint64_t>(size) > DSPStream::max_buffer_bytes / sizeof(T))
                        return RES_ERR;
                    buffer_size = static_cast<int>(size);
                }
                else
                    throw satdump_exception(key);
                return RES_OK;
            }
        };
    } // namespace ndsp
} // namespace satdump
