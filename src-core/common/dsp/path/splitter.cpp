#include "splitter.h"
#include <volk/volk.h>

namespace dsp
{
    SplitterBlock::SplitterBlock(std::shared_ptr<dsp::stream<complex_t>> input)
        : Block(input)
    {
    }

    // Fork: stop() also releases an unread FFT or VFO output before joining the worker. This
    // works when consumers stop first, or never started, without requiring them to keep reading.
    void SplitterBlock::stop()
    {
        stop_with_outputs([&]()
        {
            output_stream->stopWriter();
            std::lock_guard<std::mutex> lock(state_mutex);
            for (auto &o : outputs)
                o.second.output_stream->stopWriter();
            for (auto &o : vfo_outputs)
                o.second.output_stream->stopWriter();
            publications.stop();
        });
    }

    // Normal Copy
    void SplitterBlock::add_output(std::string id)
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        if (outputs.count(id) == 0)
            outputs.insert({id, {std::make_shared<dsp::stream<complex_t>>(), false}});
    }

    void SplitterBlock::del_output(std::string id)
    {
        std::unique_lock<std::mutex> lock(state_mutex);
        auto it = outputs.find(id);
        if (it != outputs.end())
        {
            auto retired = it->second.output_stream;
            outputs.erase(it);
            publications.retire(lock, retired);
        }
    }

    std::shared_ptr<dsp::stream<complex_t>> SplitterBlock::get_output(std::string id)
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        auto it = outputs.find(id);
        return it == outputs.end() ? nullptr : it->second.output_stream;
    }

    void SplitterBlock::set_enabled(std::string id, bool enable)
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

    void SplitterBlock::reset_output(std::string id)
    {
        std::unique_lock<std::mutex> lock(state_mutex);
        auto it = outputs.find(id);
        if (it != outputs.end())
        {
            auto replacement = std::make_shared<dsp::stream<complex_t>>();
            auto retired = it->second.output_stream;
            it->second.output_stream = replacement;
            it->second.enabled = false;
            publications.retire(lock, retired);
        }
    }

    // VFO
    void SplitterBlock::add_vfo(std::string id, double samplerate, double freq)
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        if (vfo_outputs.count(id) == 0)
            vfo_outputs.insert({id,
                                {std::make_shared<dsp::stream<complex_t>>(),
                                 false, (float)freq, complex_t(cos(hz_to_rad(freq, samplerate)), sin(hz_to_rad(freq, samplerate)))}});
    }

    void SplitterBlock::del_vfo(std::string id)
    {
        std::unique_lock<std::mutex> lock(state_mutex);
        auto it = vfo_outputs.find(id);
        if (it != vfo_outputs.end())
        {
            auto retired = it->second.output_stream;
            vfo_outputs.erase(it);
            publications.retire(lock, retired);
        }
    }

    std::shared_ptr<dsp::stream<complex_t>> SplitterBlock::get_vfo_output(std::string id)
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        auto it = vfo_outputs.find(id);
        return it == vfo_outputs.end() ? nullptr : it->second.output_stream;
    }

    void SplitterBlock::set_vfo_enabled(std::string id, bool enable)
    {
        std::unique_lock<std::mutex> lock(state_mutex);
        auto it = vfo_outputs.find(id);
        if (it != vfo_outputs.end())
        {
            it->second.enabled = enable;
            if (!enable)
                publications.wait(lock, it->second.output_stream);
        }
    }

    void SplitterBlock::reset_vfo(std::string id)
    {
        std::unique_lock<std::mutex> lock(state_mutex);
        auto it = vfo_outputs.find(id);
        if (it != vfo_outputs.end())
        {
            auto replacement = std::make_shared<dsp::stream<complex_t>>();
            auto retired = it->second.output_stream;
            it->second.output_stream = replacement;
            it->second.enabled = false;
            publications.retire(lock, retired);
        }
    }

    // Main
    void SplitterBlock::set_main_enabled(bool enable)
    {
        std::unique_lock<std::mutex> lock(state_mutex);
        enable_main = enable;
        if (!enable)
            publications.wait(lock, output_stream);
    }

    void SplitterBlock::work()
    {
        int nsamples = input_stream->read();
        if (nsamples <= 0)
        {
            input_stream->flush();
            return;
        }

        // Fork: retain the destinations while publishing, but release the configuration lock
        // before any swap() can wait. stop() must acquire it to wake those waits.
        std::vector<std::shared_ptr<dsp::stream<complex_t>>> destinations;
        SplitterPublications::Batch batch;
        {
            std::lock_guard<std::mutex> lock(state_mutex);

            // Main
            if (enable_main)
                memcpy(output_stream->writeBuf, input_stream->readBuf, nsamples * sizeof(complex_t));

            // Copy outputs
            for (auto &o : outputs)
                if (o.second.enabled)
                    memcpy(o.second.output_stream->writeBuf, input_stream->readBuf, nsamples * sizeof(complex_t));

            // VFO Outputs
            for (auto &o : vfo_outputs)
            {
                if (o.second.enabled)
                {
                    if (o.second.freq == 0)
                        memcpy(o.second.output_stream->writeBuf, input_stream->readBuf, nsamples * sizeof(complex_t));
                    else
#if VOLK_VERSION >= 030100
                        volk_32fc_s32fc_x2_rotator2_32fc((lv_32fc_t *)o.second.output_stream->writeBuf, (lv_32fc_t *)input_stream->readBuf, (lv_32fc_t *)&o.second.phase_delta, (lv_32fc_t *)&o.second.phase, nsamples);
#else
                        volk_32fc_s32fc_x2_rotator_32fc((lv_32fc_t *)o.second.output_stream->writeBuf, (lv_32fc_t *)input_stream->readBuf, o.second.phase_delta, (lv_32fc_t *)&o.second.phase, nsamples);
#endif
                }
            }

            if (enable_main)
                destinations.push_back(output_stream);
            for (auto &o : outputs)
                if (o.second.enabled)
                    destinations.push_back(o.second.output_stream);
            for (auto &o : vfo_outputs)
                if (o.second.enabled)
                    destinations.push_back(o.second.output_stream);
            batch = publications.select(destinations);
            // Fork: queued publications must be the only owners outside the configuration.
            destinations.clear();
        }

        input_stream->flush();
        publications.publish(batch, nsamples, state_mutex);
    }
}
