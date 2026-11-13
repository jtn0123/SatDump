#pragma once

#include <stdexcept>

// Fork: exit codes of the satdump command (FORK.md lists them all): 0 finished, 1 usage or
// settings error, 2 the command ran and failed, 3 a radio problem (the radio is not there, or
// could not be opened, set up or started).
namespace satdump
{
    constexpr int EXIT_RADIO = 3;

    // A radio that failed to start, told apart from the other errors of the same start-up step
    struct RadioError : std::runtime_error
    {
        using std::runtime_error::runtime_error;
    };

    template <typename Source>
    void start_radio(Source &source)
    {
        try
        {
            source->start();
        }
        catch (std::exception &e)
        {
            throw RadioError(e.what());
        }
    }

    // Fork: a radio stop may join a receive callback blocked on a full output. Wake that writer
    // before joining it, also when setup failed before any downstream reader was started.
    template <typename Source>
    void stop_radio(Source &source)
    {
        if (source->output_stream)
            source->output_stream->stopWriter();
        source->stop();
    }
} // namespace satdump
