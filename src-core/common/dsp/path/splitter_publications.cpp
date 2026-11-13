#include "splitter_publications.h"
#include <algorithm>

namespace dsp
{
    SplitterPublications::Batch SplitterPublications::select(const std::vector<std::shared_ptr<stream<complex_t>>> &outputs)
    {
        Batch batch;
        for (const auto &output : outputs)
        {
            auto publication = std::make_shared<Publication>();
            publication->output = output;
            batch.push_back(publication);
        }
        pending = batch;
        return batch;
    }

    void SplitterPublications::publish(Batch &batch, int samples, std::mutex &mutex)
    {
        try
        {
            for (auto &publication : batch)
            {
                std::shared_ptr<stream<complex_t>> output;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (publication->finished)
                        continue; // Fork: retirement cancelled this queued publication.
                    publication->active = true;
                    output = publication->output;
                }
                output->swap(samples);
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    publication->active = false;
                    publication->finished = true;
                    publication->output.reset();
                }
                output.reset();
                done.notify_all();
            }
        }
        catch (...)
        {
            // Fork: release waiters on the exact batch that failed, then let the block report it.
            {
                std::lock_guard<std::mutex> lock(mutex);
                for (auto &publication : batch)
                {
                    publication->active = false;
                    publication->finished = true;
                    publication->output.reset();
                }
            }
            done.notify_all();
            throw;
        }
    }

    void SplitterPublications::wait(std::unique_lock<std::mutex> &lock, std::shared_ptr<stream<complex_t>> output)
    {
        // Fork: a retained output waits only for publications selected before disable. A
        // concurrent re-enable must not add future batches to this barrier.
        Batch selected;
        for (auto &publication : pending)
            if (publication->output == output)
                selected.push_back(publication);
        done.wait(lock, [&]()
        {
            return std::all_of(selected.begin(), selected.end(), [](const auto &publication) { return publication->finished; });
        });
    }

    void SplitterPublications::retire(std::unique_lock<std::mutex> &lock, std::shared_ptr<stream<complex_t>> output)
    {
        output->stopWriter();
        for (auto &publication : pending)
        {
            if (publication->output == output && !publication->active)
            {
                // Fork: cancel and release queued publications immediately, even if another
                // output is blocking the worker earlier in this batch.
                publication->finished = true;
                publication->output.reset();
            }
        }
        done.notify_all();
        wait(lock, output); // Fork: only an active swap can still refer to this retired stream.
    }

    void SplitterPublications::stop()
    {
        for (auto &publication : pending)
        {
            if (publication->output)
                publication->output->stopWriter();
            if (!publication->active)
            {
                publication->finished = true;
                publication->output.reset();
            }
        }
        done.notify_all();
    }
}
