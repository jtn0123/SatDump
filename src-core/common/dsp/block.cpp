#include "block.h"
#include "block_failures.h"
#include <atomic>
#include <mutex>

namespace dsp
{
    // Fork: see block_failures.h
    namespace
    {
        std::atomic<uint64_t> &failure_count()
        {
            static std::atomic<uint64_t> count{0};
            return count;
        }

        std::mutex &failure_mtx()
        {
            static std::mutex mtx;
            return mtx;
        }

        std::string &failure_last()
        {
            static std::string last;
            return last;
        }
    } // namespace

    void record_block_failure(const std::string &block, const std::string &error)
    {
        {
            std::lock_guard<std::mutex> lg(failure_mtx());
            failure_last() = block + ": " + error;
        }
        failure_count()++;
    }

    uint64_t block_failure_count() { return failure_count(); }

    std::string last_block_failure()
    {
        std::lock_guard<std::mutex> lg(failure_mtx());
        return failure_last();
    }

    float branchless_clip(float x, float clip) { return 0.5 * (std::abs(x + clip) - std::abs(x - clip)); }

    float branched_clip(float x, float clip)
    {
        if (x < -clip)
            return -clip;
        else if (x > clip)
            return clip;
        else
            return x;
    }

    double hz_to_rad(double freq, double samplerate) { return 2.0 * M_PI * (freq / samplerate); }

    double rad_to_hz(double rad, double samplerate) { return (rad / (2.0 * M_PI)) * samplerate; }
}; // namespace dsp