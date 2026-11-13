#include "splitter.h"
#include <volk/volk.h>
#include <algorithm>

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
            for (auto &destination : publishing_outputs)
                destination->stopWriter();
        });
    }

    // Fork: disabled streams keep their identity and writer state, as upstream users expect
    // when enabling the same recording/FFT consumer again. Wait until any selected publication
    // has finished before allowing that consumer to stop. Retired streams are woken first.
    void SplitterBlock::wait_for_publication(std::unique_lock<std::mutex> &lock, std::shared_ptr<dsp::stream<complex_t>> destination)
    {
        publication_done.wait(lock, [&]()
        {
            return std::find(publishing_outputs.begin(), publishing_outputs.end(), destination) == publishing_outputs.end();
        });
    }

    // Normal Copy
    void SplitterBlock::add_output(std::string id)
    {
        std::lock_guard<std::mutex> change(config_mutex);
        std::lock_guard<std::mutex> lock(state_mutex);
        if (outputs.count(id) == 0)
            outputs.insert({id, {std::make_shared<dsp::stream<complex_t>>(), false}});
    }

    void SplitterBlock::del_output(std::string id)
    {
        std::lock_guard<std::mutex> change(config_mutex);
        std::unique_lock<std::mutex> lock(state_mutex);
        auto it = outputs.find(id);
        if (it != outputs.end())
        {
            it->second.enabled = false;
            it->second.output_stream->stopWriter();
            wait_for_publication(lock, it->second.output_stream);
            outputs.erase(it);
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
        std::lock_guard<std::mutex> change(config_mutex);
        std::unique_lock<std::mutex> lock(state_mutex);
        auto it = outputs.find(id);
        if (it != outputs.end())
        {
            it->second.enabled = enable;
            if (!enable)
                wait_for_publication(lock, it->second.output_stream);
        }
    }

    void SplitterBlock::reset_output(std::string id)
    {
        std::lock_guard<std::mutex> change(config_mutex);
        std::unique_lock<std::mutex> lock(state_mutex);
        auto it = outputs.find(id);
        if (it != outputs.end())
        {
            auto replacement = std::make_shared<dsp::stream<complex_t>>();
            it->second.enabled = false;
            it->second.output_stream->stopWriter();
            wait_for_publication(lock, it->second.output_stream);
            it->second.output_stream = replacement;
        }
    }

    // VFO
    void SplitterBlock::add_vfo(std::string id, double samplerate, double freq)
    {
        std::lock_guard<std::mutex> change(config_mutex);
        std::lock_guard<std::mutex> lock(state_mutex);
        if (vfo_outputs.count(id) == 0)
            vfo_outputs.insert({id,
                                {std::make_shared<dsp::stream<complex_t>>(),
                                 false, (float)freq, complex_t(cos(hz_to_rad(freq, samplerate)), sin(hz_to_rad(freq, samplerate)))}});
    }

    void SplitterBlock::del_vfo(std::string id)
    {
        std::lock_guard<std::mutex> change(config_mutex);
        std::unique_lock<std::mutex> lock(state_mutex);
        auto it = vfo_outputs.find(id);
        if (it != vfo_outputs.end())
        {
            it->second.enabled = false;
            it->second.output_stream->stopWriter();
            wait_for_publication(lock, it->second.output_stream);
            vfo_outputs.erase(it);
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
        std::lock_guard<std::mutex> change(config_mutex);
        std::unique_lock<std::mutex> lock(state_mutex);
        auto it = vfo_outputs.find(id);
        if (it != vfo_outputs.end())
        {
            it->second.enabled = enable;
            if (!enable)
                wait_for_publication(lock, it->second.output_stream);
        }
    }

    void SplitterBlock::reset_vfo(std::string id)
    {
        std::lock_guard<std::mutex> change(config_mutex);
        std::unique_lock<std::mutex> lock(state_mutex);
        auto it = vfo_outputs.find(id);
        if (it != vfo_outputs.end())
        {
            auto replacement = std::make_shared<dsp::stream<complex_t>>();
            it->second.enabled = false;
            it->second.output_stream->stopWriter();
            wait_for_publication(lock, it->second.output_stream);
            it->second.output_stream = replacement;
        }
    }

    // Main
    void SplitterBlock::set_main_enabled(bool enable)
    {
        std::lock_guard<std::mutex> change(config_mutex);
        std::unique_lock<std::mutex> lock(state_mutex);
        enable_main = enable;
        if (!enable)
            wait_for_publication(lock, output_stream);
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
            publishing_outputs = destinations;
        }

        try
        {
            input_stream->flush();
            for (auto &destination : destinations)
            {
                destination->swap(nsamples);
                {
                    std::lock_guard<std::mutex> lock(state_mutex);
                    auto it = std::find(publishing_outputs.begin(), publishing_outputs.end(), destination);
                    if (it != publishing_outputs.end())
                        publishing_outputs.erase(it);
                }
                destination.reset();
                publication_done.notify_all();
            }
        }
        catch (...)
        {
            // Fork: release publication barriers even when the block's worker fails.
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                publishing_outputs.clear();
            }
            publication_done.notify_all();
            throw;
        }
    }
}
