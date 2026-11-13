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
            work2Thread = std::thread(&ReshapeBufferBlock::work2, this);
        }

        template <typename T>
        void ReshapeBufferBlock<T>::stop_extra() noexcept
        {
            // Fork: after EOF the output worker must finish complete chunks before it joins.
            // A stop without EOF (or a partial start) cancels its pending read instead.
            if (!ring_buf.writerFinished())
            {
                ring_buf.stopWriter();
                ring_buf.stopReader();
            }
            if (work2Thread.joinable())
                work2Thread.join();
        }

        template <typename T>
        bool ReshapeBufferBlock<T>::work()
        {
            DSPBuffer iblk = inputs[0].fifo->wait_dequeue();

            if (iblk.isTerminator())
            {
                // Fork: signal completion instead of racing the output worker to publish EOF.
                propagate_eof = iblk.terminatorShouldPropagate();
                inputs[0].fifo->free(iblk);
                ring_buf.finishWriter();
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
            // Fork: init() sizes the staging chunk before workers start. Configuration changes
            // take effect on restart, so this worker never reads beyond that allocation.
            const int bufsize = static_cast<int>(output_chunk.size());
            while (true)
            {
                int read = ring_buf.read(output_chunk.data(), bufsize);
                if (read != bufsize)
                {
                    // Fork: upstream never published an incomplete chunk. Discard that tail;
                    // EOF follows all complete chunks, on this one producer thread only.
                    if (read >= 0 && propagate_eof)
                        outputs[0].fifo->wait_enqueue(outputs[0].fifo->newBufferTerminator());
                    return;
                }
                DSPBuffer oblk = outputs[0].fifo->newBufferSamples(bufsize, sizeof(T));
                memcpy(oblk.getSamples<T>(), output_chunk.data(), bufsize * sizeof(T));
                oblk.size = bufsize;
                outputs[0].fifo->wait_enqueue(oblk);
            }
        }

        template class ReshapeBufferBlock<int8_t>;
        template class ReshapeBufferBlock<float>;
        template class ReshapeBufferBlock<complex_t>;
    } // namespace ndsp
} // namespace satdump
