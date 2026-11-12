#include "task_scheduler.h"
#include "core/plugin.h"
#include "logger.h"
#include <chrono>
#include <limits>
#include <thread>

namespace satdump
{
    void TaskScheduler::thread_func()
    {
        while (running)
        {
            // Lock the mutex and set up
            std::unique_lock<std::mutex> lock(task_mtx);
            // Fork: wake at least once a day. An unbounded wait (time_t max seconds) overflowed
            // when std::chrono converted it to nanoseconds; with no task due, the loop just repeats.
            time_t sleep_for = MAX_SLEEP;
            time_t wait_began = time(0);
            ScheduledTask *task_to_run = nullptr;
            std::string task_name;

            // Look for next task to run
            for (auto &this_task : scheduled_tasks)
            {
                // Fork: an interval too large to add is never due (no signed overflow)
                const ScheduledTask &t = this_task.second;
                if (t.run_interval > std::numeric_limits<time_t>::max() - t.last_run)
                    continue;

                // This task is already due! Run it
                time_t next_run = t.last_run + t.run_interval;
                if (next_run <= wait_began)
                {
                    task_to_run = &this_task.second;
                    task_name = this_task.first;
                    sleep_for = 0;
                    break;
                }

                // This task is the next one to run. Fork: compare with the time left until it
                // (it compared with a negative number, so the last task listed won, not the soonest)
                else if (next_run - wait_began < sleep_for)
                {
                    sleep_for = next_run - wait_began;
                    task_to_run = &this_task.second;
                    task_name = this_task.first;
                }
            }

            // Sleep until next task, or we are woken up
            needs_update = false;
            if (sleep_for > 0)
                cv.wait_for(lock, std::chrono::seconds(sleep_for), [this] { return !running || needs_update; });
            std::this_thread::sleep_for(std::chrono::milliseconds(10)); // TODOREWORK. Hogs CPU otherwise...

            // Stop/Restart loop if needed
            if (!running || needs_update ||          // We were woken up because of a change in the program
                task_to_run == nullptr ||            // Spurious wake when no task scheduled
                wait_began + sleep_for > time(NULL)) // Spurious wake
                continue;

            // Run the task that should be due now, and mark it for next run. Fork: a task that throws
            // is logged and tried again at its next run. The error used to leave this thread, which
            // ended the whole program (a failed Kepler or IERS update could do that during a pass)
            try
            {
                eventBus->fire_event(task_to_run->evt.get(), task_to_run->evt_name);
            }
            catch (const std::exception &e)
            {
                logger->error("Scheduled task %s failed: %s", task_name.c_str(), e.what());
            }
            catch (...)
            {
                logger->error("Scheduled task %s failed with an unknown error", task_name.c_str());
            }
            task_to_run->last_run = time(NULL);
        }
    }

    TaskScheduler::TaskScheduler() {}

    void TaskScheduler::start_thread() { task_thread = std::thread(&TaskScheduler::thread_func, this); }

    TaskScheduler::~TaskScheduler()
    {
        {
            // Fork: under the lock, so the thread cannot miss the wake-up between its check and its wait
            std::lock_guard<std::mutex> lock(task_mtx);
            running = false;
        }
        cv.notify_one();
        if (task_thread.joinable())
            task_thread.join();
    }
} // namespace satdump