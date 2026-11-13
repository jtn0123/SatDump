#pragma once

#include "common/dsp/buffer.h"
#include <condition_variable>
#include <memory>
#include <mutex>
#include <vector>

namespace dsp
{
    // Fork: shared publication bookkeeping for both legacy splitters. All state changes use
    // the owner's state mutex. Waits release it; retirement never waits for a queued output.
    class SplitterPublications
    {
        struct Publication
        {
            std::shared_ptr<stream<complex_t>> output;
            bool active = false;
            bool finished = false;
        };

    public:
        using Batch = std::vector<std::shared_ptr<Publication>>;
        Batch select(const std::vector<std::shared_ptr<stream<complex_t>>> &outputs);
        void publish(Batch &batch, int samples, std::mutex &mutex);
        void wait(std::unique_lock<std::mutex> &lock, std::shared_ptr<stream<complex_t>> output);
        void retire(std::unique_lock<std::mutex> &lock, std::shared_ptr<stream<complex_t>> output);
        void stop();

    private:
        Batch pending;
        std::condition_variable done;
    };
}
