#include "doctest.h"
#include "pipeline/modules/base/filestream_to_filestream.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <unistd.h>
#include <vector>

using satdump::pipeline::base::FileStreamToFileStreamModule;

namespace
{
    // The smallest module that exposes read_data(); its output is switched off
    struct Reader : FileStreamToFileStreamModule
    {
        explicit Reader(const std::string &in) : FileStreamToFileStreamModule(in, "", nlohmann::json::object()) { fsfsm_enable_output = false; }
        void process() override {}
        void drawUI(bool) override {}
        std::string getIDM() override { return "test_reader"; }
        using FileStreamToFileStreamModule::init;
        using FileStreamToFileStreamModule::read_data;
    };

    const uint8_t STALE = 0xEE;
} // namespace

TEST_CASE("read_data: a short read from a file returns its size and zero-fills the rest")
{
    const std::filesystem::path f = std::filesystem::temp_directory_path() / ("satdump-fsfs-" + std::to_string(::getpid()) + ".bin");
    std::ofstream(f, std::ios::binary).write(std::string(100, '\x11').data(), 100);

    Reader r(f.string());
    r.setInputType(satdump::pipeline::DATA_FILE);
    r.init();

    std::vector<uint8_t> buf(64, STALE);
    CHECK(r.read_data(buf.data(), 64) == 64);
    CHECK(buf == std::vector<uint8_t>(64, 0x11));

    buf.assign(64, STALE); // the 36 bytes left, then zeros (never the previous buffer)
    CHECK(r.read_data(buf.data(), 64) == 36);
    CHECK(std::vector<uint8_t>(buf.begin(), buf.begin() + 36) == std::vector<uint8_t>(36, 0x11));
    CHECK(std::vector<uint8_t>(buf.begin() + 36, buf.end()) == std::vector<uint8_t>(28, 0));

    buf.assign(64, STALE);
    CHECK(r.read_data(buf.data(), 64) == 0);
    CHECK(buf == std::vector<uint8_t>(64, 0));
    std::filesystem::remove(f);
}

TEST_CASE("read_data: a stream stopped part-way keeps the bytes it delivered and zero-fills the rest")
{
    Reader r("");
    r.setInputType(satdump::pipeline::DATA_STREAM);
    r.input_fifo = std::make_shared<dsp::RingBuffer<uint8_t>>(1000);
    r.init();

    std::vector<uint8_t> part(100, 0x22);
    r.input_fifo->write(part.data(), part.size());
    std::thread stopper([&r]()
                        {
                            std::this_thread::sleep_for(std::chrono::milliseconds(100));
                            r.input_fifo->stopReader(); });

    std::vector<uint8_t> buf(512, STALE);
    r.read_data(buf.data(), 512); // blocks for the missing 412 bytes until stopped
    stopper.join();
    CHECK(std::vector<uint8_t>(buf.begin(), buf.begin() + 100) == part);
    CHECK(std::vector<uint8_t>(buf.begin() + 100, buf.end()) == std::vector<uint8_t>(412, 0));
}
