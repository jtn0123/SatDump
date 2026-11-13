#pragma once

#include "dsp_buffer.h"
#include "readerwritercircularbuffer.h"
#include <atomic>
#include <limits>
#include <new>
#include <stdexcept>

namespace satdump
{
    namespace ndsp
    {
        class DSPStream
        {
        private:
            // Fork: injectable allocator for deterministic failure tests, without changing stream layout.
            friend struct DSPStreamTestAccess;
            using Allocator = void *(*)(size_t, size_t);
            static inline std::atomic<Allocator> allocator{volk_malloc};
            moodycamel::BlockingReaderWriterCircularBuffer<DSPBuffer> fifo;
            moodycamel::BlockingReaderWriterCircularBuffer<DSPBuffer> ufifo;

        public:
            DSPStream(int size) : fifo(size), ufifo(size)
            {
                for (int i = 0; i < size; i++)
                {
                    DSPBuffer b;
                    ufifo.wait_enqueue(b);
                }
            }

            ~DSPStream()
            {
                DSPBuffer b;
                while (fifo.try_dequeue(b))
                    if (b.ptr != nullptr)
                        volk_free(b.ptr);
                while (ufifo.try_dequeue(b))
                    if (b.ptr != nullptr)
                        volk_free(b.ptr);
            }

            inline void wait_enqueue(DSPBuffer b) { fifo.wait_enqueue(b); }

            inline DSPBuffer wait_dequeue()
            {
                DSPBuffer b;
                fifo.wait_dequeue(b);
                return b;
            }

            inline bool try_dequeue(DSPBuffer &b) { return fifo.try_dequeue(b); }

            DSPBuffer alloc(size_t size)
            {
                if (size > max_buffer_bytes)
                    throw std::length_error("DSP buffer exceeds 64 MiB");
                DSPBuffer b;
                ufifo.wait_dequeue(b);
                // Fork: allocate before releasing the old storage, and recycle the token on
                // failure. EOF needs no allocation, even when memory has run out.
                if (size > b.ptr_size)
                {
                    void *replacement = allocator.load()(size, volk_get_alignment());
                    if (!replacement)
                    {
                        free(b);
                        throw std::bad_alloc();
                    }
                    volk_free(b.ptr);
                    b.ptr = replacement;
                    b.ptr_size = size;
                }
                return b;
            }

            inline void free(DSPBuffer &b) { ufifo.wait_enqueue(b); }

            size_t size_approx() { return fifo.size_approx(); }
            size_t max_capacity() { return fifo.max_capacity(); }

        public:
            DSPBuffer newBufferTerminator(bool prop = true)
            {
                DSPBuffer b = alloc(0);
                b.type = prop ? DSP_BUFFER_TYPE_TERMINATOR_PROPAGATING : DSP_BUFFER_TYPE_TERMINATOR_NON_PROPAGATING;
                return b;
            }

            // Fork: cap each stream/staging allocation at 64 MiB (8M complex samples),
            // well above normal DSP chunks, while bounding queue memory and rejecting wraparound.
            static constexpr size_t max_buffer_bytes = 64 * 1024 * 1024;
            static size_t checked_sample_bytes(size_t size, size_t typesize)
            {
                if (typesize == 0 || size > std::numeric_limits<size_t>::max() / typesize ||
                    size * typesize > max_buffer_bytes)
                    throw std::length_error("DSP buffer exceeds 64 MiB or byte count overflows");
                return size * typesize;
            }

            DSPBuffer newBufferSamples(uint32_t size, int typesize)
            {
                if (typesize <= 0)
                    throw std::length_error("DSP sample size must be positive");
                DSPBuffer b = alloc(checked_sample_bytes(size, static_cast<size_t>(typesize)));
                b.type = DSP_BUFFER_TYPE_SAMPLES;
                b.max_size = size;
                return b;
            }

            /**
             * @brief Queue a terminator, but only if a free buffer is
             * available right now.
             * Fork: wait_enqueue(newBufferTerminator()) waits for a free buffer,
             * forever if the reader has stopped handing buffers back. A failed
             * block uses this instead so that it never hangs on a dead reader.
             * @param prop Whether this terminator should be propagating
             * @return true if the terminator was queued
             */
            bool try_send_terminator(bool prop = true)
            {
                DSPBuffer b;
                if (!ufifo.try_dequeue(b))
                    return false;
                b.type = prop ? DSP_BUFFER_TYPE_TERMINATOR_PROPAGATING : DSP_BUFFER_TYPE_TERMINATOR_NON_PROPAGATING;
                b.size = 0;
                if (fifo.try_enqueue(b))
                    return true;
                ufifo.wait_enqueue(b); // Never waits: the buffer was just taken from there
                return false;
            }
        };
    } // namespace ndsp
} // namespace satdump