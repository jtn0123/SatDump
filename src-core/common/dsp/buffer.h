#pragma once
#include <atomic>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <volk/volk.h>
#include <string.h>
#include "common/dsp/complex.h"
#include "dll_export.h"
#include <atomic>

namespace dsp
{
    // Default buffer sizes
    SATDUMP_DLL extern int STREAM_BUFFER_SIZE;
    SATDUMP_DLL extern int RING_BUF_SZ;

    /*
    Util function to create a volk aligned buffer
    */
    template <typename T>
    T *create_volk_buffer(int size, bool zero = true)
    {
        T *buffer = (T *)volk_malloc(size * sizeof(T), volk_get_alignment());
        if (zero)
            for (int i = 0; i < size; i++)
                buffer[i] = 0;
        return buffer;
    }

    template <class T>
    class stream
    {
    public:
        stream(int stream_size = STREAM_BUFFER_SIZE)
        {
            writeBuf = create_volk_buffer<T>(stream_size);
            readBuf = create_volk_buffer<T>(stream_size);

            for (int i = 0; i < stream_size; i++)
            {
                writeBuf[i] = 0;
                readBuf[i] = 0;
            }
        }

        ~stream()
        {
            volk_free(writeBuf);
            volk_free(readBuf);
        }

        bool swap(int size)
        {
            {
                // Wait to either swap or stop
                std::unique_lock<std::mutex> lck(swapMtx);
                swapCV.wait(lck, [this]
                            { return (canSwap || writerStop); });

                // If writer was stopped, abandon operation
                if (writerStop)
                {
                    return false;
                }

                // Swap buffers
                dataSize = size;
                T *temp = writeBuf;
                writeBuf = readBuf;
                readBuf = temp;
                canSwap = false;
            }

            // Notify reader that some data is ready
            {
                std::lock_guard<std::mutex> lck(rdyMtx);
                dataReady = true;
            }
            rdyCV.notify_all();

            return true;
        }

        // Returns the number of samples ready in readBuf, or -1 when there are none: the reader
        // was stopped (stopReader()) or, since the fork, the writer failed (setWriterFailed()).
        // A reader must treat any value <= 0 as "no data", then flush().
        int read()
        {
            // Wait for data to be ready or to be stopped
            std::unique_lock<std::mutex> lck(rdyMtx);
            rdyCV.wait(lck, [this]
                       { return (dataReady || readerStop || writerFailed); });

            if (readerStop)
                return -1;
            if (dataReady)
                return dataSize;

            // Fork: the writer failed and will send nothing more (setWriterFailed()). Say so
            // with -1 like a stopped reader, but only after a short wait: readers call read()
            // in a loop and would otherwise spin a CPU core until they are stopped.
            rdyCV.wait_for(lck, std::chrono::milliseconds(10), [this]
                           { return readerStop; });
            return -1;
        }

        /*
        Fork: called by a writer that failed and will never swap again (a dsp::Block whose
        work() threw). read() then returns -1 instead of waiting forever for data, and a
        dsp::Block reading this stream passes the failure on to its own output and ends.
        */
        void setWriterFailed()
        {
            {
                std::lock_guard<std::mutex> lck(rdyMtx);
                writerFailed = true;
            }
            rdyCV.notify_all();
        }

        void clearWriterFailed()
        {
            std::lock_guard<std::mutex> lck(rdyMtx);
            writerFailed = false;
        }

        // Fork: true once the writer failed and the last data it sent has been read
        bool writerFailedAndEmpty()
        {
            if (!writerFailed)
                return false;
            std::lock_guard<std::mutex> lck(rdyMtx);
            return writerFailed && !dataReady;
        }

        void flush()
        {
            // Clear data ready
            {
                std::lock_guard<std::mutex> lck(rdyMtx);
                dataReady = false;
            }

            // Notify writer that buffers can be swapped
            {
                std::lock_guard<std::mutex> lck(swapMtx);
                canSwap = true;
            }

            swapCV.notify_all();
        }

        void stopWriter()
        {
            {
                std::lock_guard<std::mutex> lck(swapMtx);
                writerStop = true;
            }
            swapCV.notify_all();
        }

        void clearWriteStop()
        {
            writerStop = false;
        }

        void stopReader()
        {
            {
                std::lock_guard<std::mutex> lck(rdyMtx);
                readerStop = true;
            }
            rdyCV.notify_all();
        }

        void clearReadStop() { readerStop = false; }
        int getDataSize() { return dataSize; }
        bool getReady() { return dataReady; }

        T *writeBuf;
        T *readBuf;

    private:
        std::mutex swapMtx;
        std::condition_variable swapCV;
        bool canSwap = true;

        std::mutex rdyMtx;
        std::condition_variable rdyCV;
        bool dataReady = false;

        bool readerStop = false;
        bool writerStop = false;
        std::atomic<bool> writerFailed{false};

        int dataSize = 0;
    };

    template <class T>
    class RingBuffer
    {
    public:
        RingBuffer()
        {
        }

        RingBuffer(int maxLatency) { init(maxLatency); }

        ~RingBuffer()
        {
            if (size != 0)
                delete[] _buffer;
            size = 0;
        }

        void init(int maxLatency)
        {
            size = RING_BUF_SZ;
            _buffer = new T[size];
            _stopReader = false;
            _stopWriter = false;
            this->maxLatency = maxLatency;
            writec = 0;
            readc = 0;
            readable = 0;
            writable = size;
            memset((void *)_buffer, 0, size * sizeof(T));
        }

        int read(T *data, int len)
        {
            int dataRead = 0;
            int toRead = 0;
            while (dataRead < len)
            {
                toRead = std::min<int>(waitUntilReadable(), len - dataRead);
                if (toRead < 0)
                {
                    return -1;
                };

                if ((toRead + readc) > size)
                {
                    memcpy(&data[dataRead], &_buffer[readc], (size - readc) * sizeof(T));
                    memcpy(&data[dataRead + (size - readc)], &_buffer[0], (toRead - (size - readc)) * sizeof(T));
                }
                else
                {
                    memcpy(&data[dataRead], &_buffer[readc], toRead * sizeof(T));
                }

                dataRead += toRead;

                _readable_mtx.lock();
                readable -= toRead;
                _readable_mtx.unlock();
                _writable_mtx.lock();
                writable += toRead;
                _writable_mtx.unlock();
                readc = (readc + toRead) % size;
                canWriteVar.notify_one();
            }
            return len;
        }

        int readAndSkip(T *data, int len, int skip)
        {
            int dataRead = 0;
            int toRead = 0;
            while (dataRead < len)
            {
                toRead = std::min<int>(waitUntilReadable(), len - dataRead);
                if (toRead < 0)
                {
                    return -1;
                };

                if ((toRead + readc) > size)
                {
                    memcpy(&data[dataRead], &_buffer[readc], (size - readc) * sizeof(T));
                    memcpy(&data[dataRead + (size - readc)], &_buffer[0], (toRead - (size - readc)) * sizeof(T));
                }
                else
                {
                    memcpy(&data[dataRead], &_buffer[readc], toRead * sizeof(T));
                }

                dataRead += toRead;

                _readable_mtx.lock();
                readable -= toRead;
                _readable_mtx.unlock();
                _writable_mtx.lock();
                writable += toRead;
                _writable_mtx.unlock();
                readc = (readc + toRead) % size;
                canWriteVar.notify_one();
            }
            dataRead = 0;
            while (dataRead < skip)
            {
                toRead = std::min<int>(waitUntilReadable(), skip - dataRead);
                if (toRead < 0)
                {
                    return -1;
                };

                dataRead += toRead;

                _readable_mtx.lock();
                readable -= toRead;
                _readable_mtx.unlock();
                _writable_mtx.lock();
                writable += toRead;
                _writable_mtx.unlock();
                readc = (readc + toRead) % size;
                canWriteVar.notify_one();
            }
            return len;
        }

        int waitUntilReadable()
        {
            if (_stopReader)
            {
                return -1;
            }
            int _r = getReadable();
            if (_r != 0)
            {
                return _r;
            }
            std::unique_lock<std::mutex> lck(_readable_mtx);
            canReadVar.wait(lck, [=]()
                            { return ((this->getReadable(false) > 0) || this->getReadStop()); });
            if (_stopReader)
            {
                return -1;
            }
            return getReadable(false);
        }

        int getReadable(bool lock = true)
        {
            if (lock)
            {
                _readable_mtx.lock();
            };
            int _r = readable;
            if (lock)
            {
                _readable_mtx.unlock();
            };
            return _r;
        }

        int write(T *data, int len)
        {
            int dataWritten = 0;
            int toWrite = 0;
            while (dataWritten < len)
            {
                toWrite = std::min<int>(waitUntilwritable(), len - dataWritten);
                if (toWrite < 0)
                {
                    return -1;
                };

                if ((toWrite + writec) > size)
                {
                    memcpy(&_buffer[writec], &data[dataWritten], (size - writec) * sizeof(T));
                    memcpy(&_buffer[0], &data[dataWritten + (size - writec)], (toWrite - (size - writec)) * sizeof(T));
                }
                else
                {
                    memcpy(&_buffer[writec], &data[dataWritten], toWrite * sizeof(T));
                }

                dataWritten += toWrite;

                _readable_mtx.lock();
                readable += toWrite;
                _readable_mtx.unlock();
                _writable_mtx.lock();
                writable -= toWrite;
                _writable_mtx.unlock();
                writec = (writec + toWrite) % size;

                canReadVar.notify_one();
            }
            return len;
        }

        int waitUntilwritable()
        {
            if (_stopWriter)
            {
                return -1;
            }
            int _w = getWritable();
            if (_w != 0)
            {
                return _w;
            }
            std::unique_lock<std::mutex> lck(_writable_mtx);
            canWriteVar.wait(lck, [=]()
                             { return ((this->getWritable(false) > 0) || this->getWriteStop()); });
            if (_stopWriter)
            {
                return -1;
            }
            return getWritable(false);
        }

        int getWritable(bool lock = true)
        {
            if (lock)
            {
                _writable_mtx.lock();
            };
            int _w = writable;
            if (lock)
            {
                _writable_mtx.unlock();
                _readable_mtx.lock();
            };
            int _r = readable;
            if (lock)
            {
                _readable_mtx.unlock();
            };
            return std::max<int>(std::min<int>(_w, maxLatency - _r), 0);
        }

        // Fork: the flag is set under the lock the other side waits with. Set without it, the
        // wake-up could come between the other side's check and its wait, and that side then waited
        // for ever (a pipeline that never stopped)
        void stopReader()
        {
            {
                std::lock_guard<std::mutex> lck(_readable_mtx);
                _stopReader = true;
            }
            canReadVar.notify_all();
        }

        void stopWriter()
        {
            {
                std::lock_guard<std::mutex> lck(_writable_mtx);
                _stopWriter = true;
            }
            canWriteVar.notify_all();
        }

        bool getReadStop() { return _stopReader; }
        bool getWriteStop() { return _stopWriter; }
        void clearReadStop() { _stopReader = false; }
        void clearWriteStop() { _stopWriter = false; }
        void setMaxLatency(int maxLatency) { this->maxLatency = maxLatency; }

    private:
        T *_buffer;
        int size = 0;
        int readc;
        int writec;
        int readable;
        int writable;
        int maxLatency;
        std::atomic<bool> _stopReader; // Fork: atomic, as they are also read without the lock
        std::atomic<bool> _stopWriter;
        std::mutex _readable_mtx;
        std::mutex _writable_mtx;
        std::condition_variable canReadVar;
        std::condition_variable canWriteVar;
    };
};