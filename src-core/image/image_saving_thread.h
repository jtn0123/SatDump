#pragma once

#include "image.h"
#include "io.h"
#include "logger.h"
#include <atomic>
#include <mutex>
#include <thread>

namespace satdump
{
    namespace image
    {
        class ImageSavingThread
        {
        private:
            std::thread saving_thread;
            std::vector<std::pair<Image, std::string>> queue;
            std::mutex queue_mutex;

            std::atomic<bool> thread_should_run{true}; // Fork: atomic, as the thread reads it without the lock

            // Fork: an image that cannot be saved is logged and the next one is saved. The error used to
            // leave this thread, which ended the whole program. The queued images are now taken all
            // at once without copying them (a copy could itself fail, for want of memory), and the
            // queue is only read under its lock
            void actual_saving_thread()
            {
                while (true)
                {
                    bool should_run = thread_should_run; // Read first, so images pushed before the end are still taken
                    std::vector<std::pair<Image, std::string>> images;
                    {
                        std::lock_guard<std::mutex> lock(queue_mutex);
                        images.swap(queue);
                    }

                    if (images.size() == 0)
                    {
                        if (!should_run)
                            break;
                        std::this_thread::sleep_for(std::chrono::seconds(1));
                        continue;
                    }

                    for (auto &img : images)
                    {
                        try
                        {
                            logger->info("Saving " + img.second);
                            save_img(img.first, img.second);
                        }
                        catch (const std::exception &e)
                        {
                            logger->error("Could not save %s: %s", img.second.c_str(), e.what());
                        }
                        catch (...)
                        {
                            logger->error("Could not save %s: unknown error", img.second.c_str());
                        }
                    }
                }
            }

        public:
            bool use_thread;

        public:
            ImageSavingThread(bool use_thread) : use_thread(use_thread)
            {
                if (use_thread)
                    saving_thread = std::thread(&ImageSavingThread::actual_saving_thread, this);
            }

            ~ImageSavingThread()
            {
                thread_should_run = false;
                if (use_thread && saving_thread.joinable())
                    saving_thread.join();
            }

            void push(Image &img, std::string path)
            {
                if (use_thread)
                {
                    std::lock_guard<std::mutex> lock(queue_mutex); // Fork: let go also when the copy fails
                    queue.push_back({img, path});
                }
                else
                {
                    logger->info("Saving " + path);
                    save_img(img, path);
                }
            }
        };
    } // namespace image
} // namespace satdump