#pragma once

// Fork: emitted only by a test build, on the command thread after the single-VFO radio,
// FFT and pipeline have started. The fault module can throw here to test that catch path
// without adding a fault flag to the installed command's normal interface.
namespace fork_test
{
    // Fork: emitted after a VFO is enabled, before the next VFO is checked.
    struct VfoStartedEvent
    {
    };

    struct LiveRunningEvent
    {
    };
}
