// Fork: deterministic resource failures must unwind without termination or lost stream tokens.
#include "doctest.h"
#include "block_lifecycle_test_access.h"
#include "core/cli/cli.h"
#include "core/plugin.h"
#include "libs/ctpl/ctpl_stl.h"
#include "log_capture.h"
#include <limits>
#include <system_error>

namespace ctpl
{
    struct ThreadPoolTestAccess
    {
        static inline int remaining = 0;
        static void fail() { if (remaining-- == 0) throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again)); }
        static void inject(int after) { remaining = after; thread_pool::before_thread = fail; }
        static void reset() { thread_pool::before_thread = nullptr; }
        ~ThreadPoolTestAccess() { reset(); }
    };
}
namespace satdump::ndsp
{
    struct DSPStreamTestAccess
    {
        static void *fail(size_t, size_t) { return nullptr; }
        static void inject() { DSPStream::allocator = fail; }
        ~DSPStreamTestAccess() { DSPStream::allocator = volk_malloc; }
    };
}
using namespace satdump::ndsp;

TEST_CASE("shutdown failures: partial pool construction joins workers")
{
    ctpl::ThreadPoolTestAccess injection;
    for (int after : {0, 1, 3})
    {
        ctpl::ThreadPoolTestAccess::inject(after);
        CHECK_THROWS_AS(ctpl::thread_pool(4), std::system_error);
    }
}

TEST_CASE("shutdown failures: failed pool growth joins workers")
{
    ctpl::ThreadPoolTestAccess injection;
    for (int after : {0, 1, 3})
    {
        ctpl::ThreadPoolTestAccess::reset();
        ctpl::thread_pool pool(2);
        CHECK(pool.push([](int) { return 42; }).get() == 42);
        ctpl::ThreadPoolTestAccess::inject(after);
        CHECK_THROWS_AS(pool.resize(6), std::system_error);
        CHECK(pool.size() == 0);
        pool.stop();
    }
}

TEST_CASE("shutdown failures: shrinking a pool joins its active removed worker")
{
    ctpl::thread_pool pool(1);
    std::atomic<bool> entered{false}, release{false}, resized{false};
    auto task = pool.push([&](int) { entered = true; while (!release) std::this_thread::yield(); });
    while (!entered) std::this_thread::yield();
    std::thread shrink([&] { pool.resize(0); resized = true; });
    // Fork: a removed worker remains owned until its task completes.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK_FALSE(resized);
    release = true;
    shrink.join();
    task.get();
    CHECK(resized);
}

TEST_CASE("shutdown failures: sample bytes reject overflow and oversized reshape state")
{
    DSPStream stream(1);
    bool rejected = false;
    try
    {
        auto unexpected = stream.newBufferSamples(536870913, sizeof(complex_t));
        stream.free(unexpected);
    }
    catch (const std::length_error &) { rejected = true; }
    CHECK(rejected);
    CHECK_THROWS_AS(DSPStream::checked_sample_bytes(std::numeric_limits<size_t>::max(), 8), std::length_error);
    CHECK_THROWS_AS(stream.newBufferSamples(1, -1), std::length_error);
    CHECK(DSPStream::checked_sample_bytes(8388608, sizeof(complex_t)) == DSPStream::max_buffer_bytes);
    // Fork: if the startup guard is removed, a sentinel prevents a huge allocation from
    // starting unconnected workers; 64 MiB + one sample is enough to test the startup cap.
    class StartupProbe : public ReshapeBufferBlock<complex_t>
    {
        void start_extra() override { throw std::runtime_error("invalid state reached startup"); }
    };
    StartupProbe block;
    CHECK(block.set_cfg("buffer_size", 536870913) == Block::RES_ERR);
    CHECK(block.get_cfg("buffer_size") == 1024);
    for (const auto &invalid : {nlohmann::json(uint64_t{4294968320}), nlohmann::json(1.5), nlohmann::json("32")})
        CHECK(block.set_cfg("buffer_size", invalid) == Block::RES_ERR);
    ReshapeBufferTestAccess::force_size(block, 8388609);
    CHECK_THROWS_AS(block.start(), std::length_error);
    CHECK_FALSE(BlockLifecycleTestAccess::main_joinable(block));
    CHECK_FALSE(ReshapeBufferTestAccess::extra_joinable(block));
    auto eof = stream.newBufferTerminator();
    stream.free(eof);
}

TEST_CASE("shutdown failures: failed allocation and growth preserve the only stream token")
{
    DSPStream stream(1);
    DSPStreamTestAccess injection;
    DSPStreamTestAccess::inject();
    CHECK_THROWS_AS(stream.newBufferSamples(4, sizeof(float)), std::bad_alloc);
    // Fork: EOF needs no allocation, and a failed growth must retain the old storage.
    auto end = stream.newBufferTerminator();
    stream.free(end);
    { DSPStreamTestAccess reset; }
    auto original = stream.newBufferSamples(4, sizeof(float));
    original.getSamples<float>()[0] = 123;
    auto ptr = original.getSamples<float>();
    stream.free(original);
    DSPStreamTestAccess::inject();
    CHECK_THROWS_AS(stream.newBufferSamples(32, sizeof(float)), std::bad_alloc);
    auto retained = stream.newBufferSamples(4, sizeof(float));
    CHECK(retained.getSamples<float>() == ptr);
    CHECK(retained.getSamples<float>()[0] == 123);
    stream.free(retained);
}

TEST_CASE("shutdown failures: reshape allocation failure records error and publishes EOF")
{
    ReshapeBufferBlock<float> block;
    block.set_cfg("buffer_size", 32);
    auto input = std::make_shared<DSPStream>(4);
    auto output = block.get_output(0, 1).fifo;
    block.set_input({"in", DSP_SAMPLE_TYPE_F32, input}, 0);
    auto samples = input->newBufferSamples(32, sizeof(float));
    samples.size = 32;
    for (int i = 0; i < 32; i++) samples.getSamples<float>()[i] = float(i);
    auto eof = input->newBufferTerminator();
    DSPStreamTestAccess injection;
    DSPStreamTestAccess::inject();
    block.start();
    input->wait_enqueue(samples);
    input->wait_enqueue(eof);
    auto end = output->wait_dequeue();
    CHECK(end.isTerminator());
    output->free(end);
    block.stop();
    CHECK(block.has_failed());
    CHECK_FALSE(block.get_failure().empty());
    CHECK_FALSE(BlockLifecycleTestAccess::main_joinable(block));
    CHECK_FALSE(ReshapeBufferTestAccess::extra_joinable(block));
}

namespace
{
    class UnknownCommand : public satdump::CmdHandler
    {
    public:
        UnknownCommand() : CmdHandler("fork_unknown") {}
        void reg(CLI::App *app) override { app->add_subcommand(cmd); }
        void run(CLI::App *, CLI::App *, bool) override { throw 123; }
    };
}
TEST_CASE("shutdown failures: command boundary reports unknown exception")
{
    if (!satdump::eventBus) satdump::eventBus = std::make_shared<satdump::EventBus>();
    satdump::eventBus->register_handler<satdump::cli::RegisterSubcommandEvent>([](auto event)
    { event.cmd_handlers.push_back(std::make_shared<UnknownCommand>()); });
    satdump::cli::CommandHandler command;
    char name[] = "satdump", sub[] = "fork_unknown";
    char *args[] = {name, sub};
    REQUIRE(command.parse(2, args) == 0);
    test_log::LogCapture logs;
    CHECK(command.run()); // Fork: main maps this failed-command result to exit 2.
    CHECK(logs.has(slog::LOG_ERROR, "Error running command : unknown exception"));
}
