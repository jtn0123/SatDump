#include "stop_signals.h"
#include <atomic>
#include <csignal>

namespace
{
    // Fork: a signal can be delivered on any thread. Only a lock-free atomic is safe both
    // between threads and inside the handler; the request is never cleared during startup.
    std::atomic<bool> requested{false};
    static_assert(std::atomic<bool>::is_always_lock_free, "the signal flag must be lock-free");

    bool installed = false; // Fork: only the CLI main thread installs and restores handlers.

    void on_stop_signal(int) { requested.store(true, std::memory_order_relaxed); }

#ifndef _WIN32
    struct sigaction previous_int;
    struct sigaction previous_term;
#else
    void (*previous_int)(int);
    void (*previous_term)(int);
#endif
}

namespace satdump
{
    namespace cli
    {
        void catch_stop_signals()
        {
            if (installed)
                return;
#ifndef _WIN32
            struct sigaction action = {};
            action.sa_handler = on_stop_signal;
            sigemptyset(&action.sa_mask);
            action.sa_flags = SA_RESTART;
            sigaction(SIGINT, &action, &previous_int);
            sigaction(SIGTERM, &action, &previous_term);
#else
            previous_int = std::signal(SIGINT, on_stop_signal);
            previous_term = std::signal(SIGTERM, on_stop_signal);
#endif
            installed = true;
        }

        bool stop_requested() { return requested.load(std::memory_order_relaxed); }

        void restore_stop_signals()
        {
            if (!installed)
                return;
#ifndef _WIN32
            sigaction(SIGINT, &previous_int, nullptr);
            sigaction(SIGTERM, &previous_term, nullptr);
#else
            std::signal(SIGINT, previous_int);
            std::signal(SIGTERM, previous_term);
#endif
            installed = false;
        }
    }
}
