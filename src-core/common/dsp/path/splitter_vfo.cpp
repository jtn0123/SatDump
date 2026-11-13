#include "splitter_vfo.h"

namespace dsp
{
    VFOSplitterBlock::VFOSplitterBlock(std::shared_ptr<dsp::stream<complex_t>> input)
        : Block(input)
    {
    }

    // Fork: use the same queued/active publication protocol as SplitterBlock. No map lock is
    // held while a publication or a frequency-shifter thread is being joined.
    void VFOSplitterBlock::stop()
    {
        stop_with_outputs([&]()
        {
            output_stream->stopWriter();
            std::lock_guard<std::mutex> lock(state_mutex);
            for (auto &output : outputs)
                output.second.output_stream->stopWriter();
            publications.stop();
        });
    }

    VFOSplitterBlock::VfoCFG VFOSplitterBlock::make_vfo(double samplerate, double frequency)
    {
        // Fork: construct privately and stop a partially started shifter before publishing it.
        auto stream = std::make_shared<dsp::stream<complex_t>>();
        auto shifter = std::make_shared<FreqShiftBlock>(stream, samplerate, frequency);
        try
        {
            shifter->start();
        }
        catch (...)
        {
            shifter->stop();
            throw;
        }
        return {stream, false, shifter, samplerate, frequency};
    }

    void VFOSplitterBlock::add_vfo(std::string id, double samplerate, double freq)
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        if (outputs.count(id) == 0)
        {
            auto vfo = make_vfo(samplerate, freq);
            try
            {
                outputs.emplace(id, vfo);
            }
            catch (...)
            {
                vfo.freq_shiter->stop();
                throw;
            }
        }
    }

    void VFOSplitterBlock::del_vfo(std::string id)
    {
        std::unique_lock<std::mutex> lock(state_mutex);
        auto it = outputs.find(id);
        if (it != outputs.end())
        {
            auto retired = it->second;
            outputs.erase(it);
            publications.retire(lock, retired.output_stream);
            lock.unlock();
            retired.freq_shiter->stop();
        }
    }

    std::shared_ptr<dsp::stream<complex_t>> VFOSplitterBlock::get_vfo_output(std::string id)
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        auto it = outputs.find(id);
        return it == outputs.end() ? nullptr : it->second.freq_shiter->output_stream;
    }

    void VFOSplitterBlock::set_vfo_enabled(std::string id, bool enable)
    {
        std::unique_lock<std::mutex> lock(state_mutex);
        auto it = outputs.find(id);
        if (it != outputs.end())
        {
            it->second.enabled = enable;
            if (!enable)
                publications.wait(lock, it->second.output_stream);
        }
    }

    void VFOSplitterBlock::reset_vfo(std::string id)
    {
        std::unique_lock<std::mutex> lock(state_mutex);
        auto it = outputs.find(id);
        if (it != outputs.end())
        {
            auto retired = it->second;
            // Fork: the replacement input needs its own shifter; the old reset left the
            // existing shifter reading the retired input forever.
            it->second = make_vfo(retired.samplerate, retired.frequency);
            publications.retire(lock, retired.output_stream);
            lock.unlock();
            retired.freq_shiter->stop();
        }
    }

    void VFOSplitterBlock::set_main_enabled(bool enable)
    {
        std::unique_lock<std::mutex> lock(state_mutex);
        enable_main = enable;
        if (!enable)
            publications.wait(lock, output_stream);
    }

    void VFOSplitterBlock::work()
    {
        int nsamples = input_stream->read();
        if (nsamples <= 0)
        {
            input_stream->flush();
            return;
        }

        SplitterPublications::Batch batch;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            std::vector<std::shared_ptr<dsp::stream<complex_t>>> destinations;
            if (enable_main)
            {
                memcpy(output_stream->writeBuf, input_stream->readBuf, nsamples * sizeof(complex_t));
                destinations.push_back(output_stream);
            }
            for (auto &output : outputs)
            {
                if (output.second.enabled)
                {
                    memcpy(output.second.output_stream->writeBuf, input_stream->readBuf, nsamples * sizeof(complex_t));
                    destinations.push_back(output.second.output_stream);
                }
            }
            batch = publications.select(destinations);
        }

        input_stream->flush();
        publications.publish(batch, nsamples, state_mutex);
    }
}
