#pragma once

/**
 * @file processing_handler.h
 */

#include "handler.h"
#include "logger.h"
#include <thread>

namespace satdump
{
    namespace handlers
    {
        /**
         * @brief ProcessingHandler base class
         *
         * Some handlers implement a lot of processing already in order
         * to allow manipulating products, images, or more. Therefore,
         * it'd be stupid not to re-use this code for headless
         * processing as well! :-)
         *
         * This is a common interface Handlers can implement if required
         * that allows implementing processing in a way common to both
         * the UI and headless uses. This implements the multi-threaded
         * processing required in the UI as well.
         *
         * @param is_processing variable to be used in the UI to disable controls
         * while processing is ongoing
         */
        class ProcessingHandler
        {
        public:
            ProcessingHandler() {}
            ~ProcessingHandler()
            {
                if (async_thread.joinable())
                    async_thread.join();
            }

        protected:
            bool is_processing = false;
            std::mutex is_processing_mtx;

            /**
             * @brief Actual processing function to be implemented by the child class
             */
            virtual void do_process() = 0;

            std::thread async_thread;

        public:
            /**
             * @brief Performing processing. This is not multi-threaded, intended to call it externally
             */
            void process()
            {
                is_processing_mtx.lock();
                is_processing = true;
                is_processing_mtx.unlock();
                // Fork: the status is cleared when processing throws too (it stayed "processing" for
                // ever, so the controls stayed disabled); the error still goes to the caller
                try
                {
                    do_process();
                }
                catch (...)
                {
                    set_is_processing(false);
                    throw;
                }
                is_processing_mtx.lock();
                is_processing = false;
                is_processing_mtx.unlock();
            }

            /**
             * @brief Set the processing status externally, to force waiting before starting a new thread
             * @param v to set if it's processing or not
             */
            void set_is_processing(bool v)
            {
                is_processing_mtx.lock();
                is_processing = v;
                is_processing_mtx.unlock();
            }

            /**
             * @brief Get the processing status externally
             * @return current status
             */
            bool get_is_processing()
            {
                is_processing_mtx.lock();
                bool s = is_processing;
                is_processing_mtx.unlock();
                return s;
            }

            /**
             * @brief Perform processing asynchronously, in a thread
             */
            void asyncProcess()
            {
                is_processing_mtx.lock();
                if (is_processing && async_thread.joinable())
                {
                    printf("ALREADY PROCESSING!!!!\n"); // TODOREWORK
                    is_processing_mtx.unlock();
                    return;
                }
                is_processing_mtx.unlock();

                // Fork: only a thread that was started is joined (joining one that never was threw, and
                // the error was dropped without a word). A join that still fails is logged, and no new
                // thread is started over the old one (that would end the program)
                if (async_thread.joinable())
                {
                    try
                    {
                        async_thread.join();
                    }
                    catch (const std::exception &e)
                    {
                        logger->error("Could not wait for the previous processing to end: %s", e.what());
                        return;
                    }
                }

                is_processing_mtx.lock();
                is_processing = true;
                is_processing_mtx.unlock();
                // Fork: an error in processing used to leave the thread and end the program
                // (std::terminate); now it is logged and the program carries on
                auto fun = [this]()
                {
                    try
                    {
                        process();
                    }
                    catch (const std::exception &e)
                    {
                        logger->error("Processing failed: %s", e.what());
                    }
                    catch (...)
                    {
                        logger->error("Processing failed with an unknown error");
                    }
                };
                async_thread = std::thread(fun);
            }
        };
    } // namespace handlers
} // namespace satdump