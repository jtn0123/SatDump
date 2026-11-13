#include "module_wrapper.h"
#include "common/dsp/block.h"
#include "common/dsp/buffer.h"
#include "common/dsp/window/window.h"
#include "dsp/block.h"
#include "pipeline/module.h"
#include <complex.h>
#include <cstdint>
#include <memory>

namespace satdump
{
    namespace ndsp
    {
        ModuleWrapperBlock::ModuleWrapperBlock() : Block("module_wrapper", {{"in", DSP_SAMPLE_TYPE_S8}}, {{"out", DSP_SAMPLE_TYPE_U8}}) {}

        ModuleWrapperBlock::~ModuleWrapperBlock()
        {
            stop_extra_before_destruction(); // Fork: join before destroying mod and its buffers.
        }

        void ModuleWrapperBlock::start_extra()
        {
            mod = pipeline::getModuleInstance(module_id, "", "/tmp/modulewrapper", mod_cfg);
            mod->input_fifo = std::make_shared<dsp::RingBuffer<uint8_t>>(1000000);
            mod->output_fifo = std::make_shared<dsp::RingBuffer<uint8_t>>(1000000);
            mod->setInputType(pipeline::DATA_STREAM);
            mod->setOutputType(pipeline::DATA_STREAM);
            module_needs_stop = true; // Fork: init may fail after acquiring resources.
            mod->init();
            mod->input_active = true;

            modThread = std::thread([this]() { mod->process(); });

            work2shouldrun = true;
            work2Thread = std::thread(&ModuleWrapperBlock::work2, this);
        }

        void ModuleWrapperBlock::stop_extra() noexcept
        {
            // Fork: stop-before-start and repeated stops do not dereference a missing module.
            if (!module_needs_stop)
                return;
            module_needs_stop = false;
            mod->input_active = false;
            try
            {
                mod->stop();
            }
            catch (const std::exception &e)
            {
                logger->error("DSP module wrapper stop failed: %s", e.what());
                dsp::record_block_failure("module_wrapper", e.what());
            }
            catch (...)
            {
                logger->error("DSP module wrapper stop failed: unknown exception");
                dsp::record_block_failure("module_wrapper", "unknown exception during stop");
            }
            work2shouldrun = false;
            mod->output_fifo->stopReader();
            mod->output_fifo->stopWriter();
            mod->input_fifo->stopReader();
            mod->input_fifo->stopWriter();
            if (work2Thread.joinable())
                work2Thread.join();
            if (modThread.joinable())
                modThread.join();
        }

        bool ModuleWrapperBlock::work()
        {
            DSPBuffer iblk = inputs[0].fifo->wait_dequeue();

            if (iblk.isTerminator())
            {
                // Fork: the main worker owns this cleanup until stop() has joined it.
                stop_extra();

                if (iblk.terminatorShouldPropagate())
                    outputs[0].fifo->wait_enqueue(outputs[0].fifo->newBufferTerminator());
                inputs[0].fifo->free(iblk);
                return true;
            }

            // printf("SEND %d\n", iblk.size);
            mod->input_fifo->write(iblk.getSamples<uint8_t>(), iblk.size);

            inputs[0].fifo->free(iblk);

            return false;
        }

        void ModuleWrapperBlock::work2()
        {
            while (work2shouldrun)
            {
                DSPBuffer oblk = outputs[0].fifo->newBufferSamples(output_read_size, sizeof(uint8_t));

                int got = mod->output_fifo->read(oblk.getSamples<uint8_t>(), oblk.max_size);

                if (got > 0)
                {
                    oblk.size = got;
                    outputs[0].fifo->wait_enqueue(oblk);
                }
                else
                {
                    outputs[0].fifo->free(oblk);
                }
            }
        }
    } // namespace ndsp
} // namespace satdump