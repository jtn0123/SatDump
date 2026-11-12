#include "selector.h"
#include "dsp/block_helpers.h"
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace satdump
{
    namespace ndsp
    {
        template <typename T>
        SelectorBlock<T>::SelectorBlock() : Block("selector_" + getShortTypeName<T>() + getShortTypeName<T>(), {{"in1", getTypeSampleType<T>()}}, {{"out1", getTypeSampleType<T>()}})
        {
        }

        template <typename T>
        SelectorBlock<T>::~SelectorBlock()
        {
        }

        template <typename T>
        bool SelectorBlock<T>::work()
        {
            // Fork: this ended on the first input that ended and left the other inputs full, so
            // the blocks writing them waited for room forever (a hang when one demodulator of
            // the audio demod failed). The other inputs are now taken and dropped until they end
            // too, outside the lock so that set_cfg() is not held up.
            std::vector<std::shared_ptr<DSPStream>> outs, others;
            {
                std::scoped_lock l(in_mtx);

                bool ended = false;
                size_t ended_id = 0;
                for (int i = 0; i < inputs.size() && !ended; i++)
                {
                    DSPBuffer iblk;
                    if (!inputs[i].fifo->try_dequeue(iblk))
                        continue;

                    if (iblk.isTerminator())
                    {
                        if (iblk.terminatorShouldPropagate())
                            outs.push_back(outputs[0].fifo);
                        inputs[i].fifo->free(iblk);
                        ended = true;
                        ended_id = i;
                        continue;
                    }

                    if (i == active_id)
                    {
                        DSPBuffer oblk = outputs[0].fifo->newBufferSamples(iblk.max_size, sizeof(T));
                        memcpy(oblk.getSamples<T>(), iblk.getSamples<T>(), iblk.size * sizeof(T));
                        oblk.size = iblk.size;
                        outputs[0].fifo->wait_enqueue(oblk);
                    }

                    inputs[i].fifo->free(iblk);
                }

                if (!ended)
                    return false;
                for (size_t j = 0; j < inputs.size(); j++)
                    if (j != ended_id && inputs[j].fifo != inputs[ended_id].fifo)
                        others.push_back(inputs[j].fifo);
            }

            end_streams(outs, others, false);
            return true;
        }

        template class SelectorBlock<complex_t>;
        template class SelectorBlock<float>;
        template class SelectorBlock<int16_t>;
        template class SelectorBlock<int8_t>;
        template class SelectorBlock<uint8_t>;
    } // namespace ndsp
} // namespace satdump