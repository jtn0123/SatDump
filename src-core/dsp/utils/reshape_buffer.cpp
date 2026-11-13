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
            std::string error;
            try
            {
                while (true)
                {
                    // Fork: each chunk keeps the size chosen at its boundary. Only this
                    // worker resizes or reads the staging vector, including live growth.
                    const int bufsize = buffer_size.load();
                    // Fork: also bound restored or corrupted state before staging allocation.
                    DSPStream::checked_sample_bytes(static_cast<size_t>(bufsize), sizeof(T));
                    output_chunk.resize(bufsize);
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
            catch (const std::exception &e)
            {
                error = e.what();
            }
            catch (...)
            {
                error = "unknown exception";
            }
            // Fork: allocation during live growth can fail. Cancel the ring so the main
            // worker keeps draining input, and report failure from this sole output producer.
            ring_buf.stopWriter();
            ring_buf.stopReader();
            record_failure(error);
            end_streams({outputs[0].fifo}, {}, true);
        }

        template class ReshapeBufferBlock<int8_t>;
        template class ReshapeBufferBlock<float>;
        template class ReshapeBufferBlock<complex_t>;
    } // namespace ndsp
} // namespace satdump
