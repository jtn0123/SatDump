#include "reshape_buffer.h"
#include "dsp/block_helpers.h"
#include <complex.h>

namespace satdump
{
    namespace ndsp
    {
        template <typename T>
        ReshapeBufferBlock<T>::ReshapeBufferBlock() : Block("reshape_buffer_" + getShortTypeName<T>() + getShortTypeName<T>(), {{"in", getTypeSampleType<T>()}}, {{"out", getTypeSampleType<T>()}})
        {
        }

        template <typename T>
        ReshapeBufferBlock<T>::~ReshapeBufferBlock()
        {
            stop_extra_before_destruction(); // Fork: join before destroying ring_buf.
        }

        template <typename T>
        void ReshapeBufferBlock<T>::start_extra()
        {
            work2shouldrun = true;
            work2Thread = std::thread(&ReshapeBufferBlock::work2, this);
        }

        template <typename T>
        void ReshapeBufferBlock<T>::stop_extra() noexcept
        {
            work2shouldrun = false;
            ring_buf.stopWriter();
            ring_buf.stopReader();
            if (work2Thread.joinable())
                work2Thread.join();
        }

        template <typename T>
        bool ReshapeBufferBlock<T>::work()
        {
            DSPBuffer iblk = inputs[0].fifo->wait_dequeue();

            if (iblk.isTerminator())
            {
                if (iblk.terminatorShouldPropagate())
                    outputs[0].fifo->wait_enqueue(outputs[0].fifo->newBufferTerminator());
                inputs[0].fifo->free(iblk);
                return true;
            }

            // Put into buffer
            ring_buf.write(iblk.getSamples<T>(), iblk.size);

            inputs[0].fifo->free(iblk);

            return false;
        }

        template <typename T>
        void ReshapeBufferBlock<T>::work2()
        {
            while (work2shouldrun)
            {
                // While we got enough elements in the buffer, output them
                const int bufsize = buffer_size;

                DSPBuffer oblk = outputs[0].fifo->newBufferSamples(bufsize, sizeof(T));
                int read = ring_buf.read(oblk.getSamples<T>(), bufsize);
                oblk.size = read;
                if (read > 0)
                    outputs[0].fifo->wait_enqueue(oblk);
                else
                    outputs[0].fifo->free(oblk);
            }
        }

        template class ReshapeBufferBlock<int8_t>;
        template class ReshapeBufferBlock<float>;
        template class ReshapeBufferBlock<complex_t>;
    } // namespace ndsp
} // namespace satdump
