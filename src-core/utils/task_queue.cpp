#include "task_queue.h"
#include "logger.h"

namespace satdump
{
    TaskQueue::TaskQueue() {}

    TaskQueue::~TaskQueue()
    {
        if (task_thread.joinable())
            task_thread.join();
    }

    void TaskQueue::threadFunc()
    {
        while (true)
        {
            std::function<void()> task;
            {
                // Fork: the queue is checked and the thread marked as ended under one lock. It used to
                // be marked after the lock was let go, and a task pushed in between was not run
                std::lock_guard<std::mutex> lock(queue_mtx);
                if (task_queue.size() == 0)
                {
                    thread_exited = true;
                    return;
                }
                task = task_queue.front();
                task_queue.pop();
            }

            // Fork: a task that fails is logged (it was dropped without a word), and one that throws
            // something other than a std::exception no longer ends the program
            try
            {
                task();
            }
            catch (const std::exception &e)
            {
                logger->error("Background task failed: %s", e.what());
            }
            catch (...)
            {
                logger->error("Background task failed with an unknown error");
            }
        }
    }

    void TaskQueue::push(std::function<void()> task)
    {
        queue_mtx.lock();

        task_queue.push(task);

        if (thread_exited)
        {
            if (task_thread.joinable())
                task_thread.join();
            thread_exited = false;
            task_thread = std::thread(&TaskQueue::threadFunc, this);
        }

        queue_mtx.unlock();
    }
} // namespace satdump
