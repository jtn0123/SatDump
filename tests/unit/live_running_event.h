#pragma once
#include <future>
#include <vector>

namespace satdump { namespace pipeline { class LivePipeline; } }

// Fork: emitted only by a test build, on the command thread after the single-VFO radio,
// FFT and pipeline have started. The fault module can throw here to test that catch path
// without adding a fault flag to the installed command's normal interface.
namespace fork_test
{
    // Fork: emitted after a VFO is enabled, before the next VFO is checked.
    struct VfoStartedEvent
    {
    };

    // Fork: main-thread hook before start_client returns, after all task futures exist.
    struct ClientStartedEvent
    {
        satdump::pipeline::LivePipeline &pipeline;
        const std::vector<std::future<void>> &tasks;
    };

    struct LiveRunningEvent
    {
    };
}
