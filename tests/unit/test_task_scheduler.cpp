#include "core/plugin.h"
#include "doctest.h"
#include "utils/task_scheduler.h"
#include <atomic>
#include <chrono>
#include <limits>
#include <memory>
#include <thread>

namespace
{
    struct SoonEvent
    {
    };
    struct LateEvent
    {
    };
    std::atomic<int> soon_runs{0}, late_runs{0};
} // namespace

TEST_CASE("Task scheduler: the task due soonest runs first, wherever it is in the list")
{
    if (!satdump::eventBus)
        satdump::eventBus = std::make_shared<satdump::EventBus>();
    satdump::eventBus->register_handler<SoonEvent>([](SoonEvent) { soon_runs++; });
    satdump::eventBus->register_handler<LateEvent>([](LateEvent) { late_runs++; });

    {
        satdump::TaskScheduler ts;
        ts.start_thread();
        const time_t now = time(nullptr);
        // Tasks are kept sorted by name: "a_" first, "z_" last. The scheduler used to wait for
        // the last task listed (an hour here) instead of the soonest (about 1 s)
        ts.add_task("a_soon", std::make_shared<SoonEvent>(), now - 9, 10);
        ts.add_task("z_late", std::make_shared<LateEvent>(), now, 3600);
        for (int i = 0; i < 60 && soon_runs == 0; i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } // stopping the scheduler must not wait for the hour-long task

    CHECK(soon_runs >= 1);
    CHECK(late_runs == 0);
}

TEST_CASE("Task scheduler: a task that is never due does not overflow the wait")
{
    // An empty scheduler, and a task whose next run is past the end of time_t; the
    // sanitizer build catches the overflow, the normal build checks the scheduler stays usable
    satdump::TaskScheduler ts;
    ts.start_thread();
    ts.add_task("never", std::make_shared<LateEvent>(), time(nullptr), std::numeric_limits<time_t>::max());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(late_runs == 0);
}
