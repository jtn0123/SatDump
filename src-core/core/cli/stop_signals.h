#pragma once

// Fork: one stop request for the whole CLI lifetime, including startup before commands run.
namespace satdump
{
    namespace cli
    {
        void catch_stop_signals();
        bool stop_requested();
        // Fork: commands without a cooperative stop retain their normal signal handling.
        void restore_stop_signals();
    }
}
