// Fork: exercise actual SDDC startup stages with fake USB operations and failed pthread_create.
#include "doctest.h"
#include "RadioHandler.h"
#include "fft_mt_r2iq.h"
#include "conv_r2iq.h"
#include "arch/linux/FX3handler.h"
#include <dlfcn.h>
#include <cerrno>
#include <system_error>

namespace
{
    thread_local int thread_budget = -1;
    int usb_streams = 0;
    int usb_starts = 0;
    class FakeUSB : public fx3class
    {
    public:
        bool streaming = false;
        bool Open(uint8_t *, uint32_t) override { return true; }
        bool Control(FX3Command, uint8_t) override { return true; }
        bool Control(FX3Command, uint32_t) override { return true; }
        bool Control(FX3Command, uint64_t) override { return true; }
        bool SetArgument(uint16_t, uint16_t) override { return true; }
        bool GetHardwareInfo(uint32_t *data) override { *data = 0; return true; }
        bool ReadDebugTrace(uint8_t *, uint8_t) override { return true; }
        void StartStream(ringbuffer<int16_t> &, int) override { streaming = true; }
        void StopStream() override { streaming = false; }
        bool Enumerate(unsigned char &, char *, uint8_t *, uint32_t) override { return true; }
    };
}

// Interpose only in this test executable, including thread creation inside the real plugin.
extern "C" int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                               void *(*fn)(void *), void *arg) noexcept
{
    using Create = int (*)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
    static auto real_create = reinterpret_cast<Create>(dlsym(RTLD_NEXT, "pthread_create"));
    if (thread_budget == 0)
    {
        thread_budget = -1;
        return EAGAIN;
    }
    if (thread_budget > 0)
        --thread_budget;
    return real_create(thread, attr, fn, arg);
}

extern "C" streaming_t *streaming_open_async(usb_device_t *, uint32_t, uint32_t,
                                             sddc_read_async_cb_t, void *)
{
    ++usb_streams;
    return reinterpret_cast<streaming_t *>(&usb_streams);
}
extern "C" int streaming_start(streaming_t *) { ++usb_starts; return 0; }
extern "C" int streaming_stop(streaming_t *) { if (usb_starts > 0) --usb_starts; return 0; }
extern "C" void streaming_close(streaming_t *) { --usb_streams; }
extern "C" int usb_device_handle_events(usb_device_t *) { std::this_thread::yield(); return 0; }

TEST_CASE("SDDC handler stops after either internal thread constructor fails")
{
    for (int budget : {0, 1})
    {
        RadioHandlerClass handler;
        auto usb = new FakeUSB;
        auto converter = new r2iqControlClass;
        REQUIRE(handler.Init(usb, [](const float *, uint32_t) {}, converter));
        CHECK(handler.Stop());
        thread_budget = budget;
        CHECK_THROWS_AS(handler.Start(0), std::system_error);
        thread_budget = -1;
        CHECK(handler.Stop());
        CHECK_FALSE(usb->streaming);
        CHECK_FALSE(converter->IsOn());
        CHECK(handler.Stop());
    }
}

TEST_CASE("SDDC FFT converter stops before Init and after worker creation fails")
{
    fft_mt_r2iq converter;
    converter.TurnOff();
    converter.TurnOff();
    ringbuffer<int16_t> input;
    ringbuffer<float> output;
    // Planning quality does not affect thread ownership; bound FFTW tuning in this test.
    fftwf_set_timelimit(0.01);
    converter.Init(1, &input, &output);
    fftwf_set_timelimit(FFTW_NO_TIMELIMIT);
    thread_budget = 0;
    CHECK_THROWS_AS(converter.TurnOn(), std::system_error);
    thread_budget = -1;
    converter.TurnOff();
    converter.TurnOff();
    CHECK_FALSE(converter.IsOn());
}

TEST_CASE("SDDC simple converter stops before Init and after worker creation fails")
{
    conv_r2iq converter;
    converter.TurnOff();
    converter.TurnOff();
    ringbuffer<int16_t> input;
    ringbuffer<float> output;
    converter.Init(1, &input, &output);
    thread_budget = 0;
    CHECK_THROWS_AS(converter.TurnOn(), std::system_error);
    thread_budget = -1;
    converter.TurnOff();
    converter.TurnOff();
    CHECK_FALSE(converter.IsOn());
}

TEST_CASE("SDDC USB stage releases a stream when poll thread creation fails")
{
    // Fork: the borrowed ring buffer must outlive its handler.
    ringbuffer<int16_t> input;
    input.setBlockSize(16);
    fx3handler usb;
    usb.StopStream();
    usb.StopStream();
    thread_budget = 0;
    CHECK_THROWS_AS(usb.StartStream(input, 4), std::system_error);
    thread_budget = -1;
    CHECK(usb_streams == 1);
    usb.StopStream();
    // Fork: after shutdown the caller can reuse the input; a second stop must not reset it.
    input.ReadDone();
    const auto read_position = input.peekReadPtr(0);
    usb.StopStream();
    CHECK(input.peekReadPtr(0) == read_position);
    CHECK(usb_streams == 0);
    CHECK(usb_starts == 0);
}

namespace
{
    struct AllocationProbe
    {
        static bool fail;
        AllocationProbe() { if (fail) throw std::bad_alloc(); }
    };
    bool AllocationProbe::fail = false;
}
TEST_CASE("SDDC ring buffer preserves ownership when startup allocation fails")
{
    ringbuffer<AllocationProbe> buffer(2);
    buffer.setBlockSize(4);
    AllocationProbe::fail = true;
    CHECK_THROWS_AS(buffer.setBlockSize(8), std::bad_alloc);
    AllocationProbe::fail = false;
    CHECK(buffer.getBlockSize() == 4);
    buffer.setBlockSize(8);
    CHECK(buffer.getBlockSize() == 8);
}
