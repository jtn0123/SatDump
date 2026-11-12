// The logger (src-core/logger.h). A message given alone is logged exactly as it is: a % in
// it (a file name, "100%", an exception text) used to be read as a printf format, which is
// undefined behaviour and can crash. Calls with arguments are printf-formatted as before;
// run-time text goes in as a %s argument, never into the format.
// Formatted messages are neither cut nor padded with zero bytes, however long they are.
// tests/compile/logger_format.sh checks which calls compile and which the compiler reports.
#include "doctest.h"
#include "log_capture.h"
#include "logger.h"
#include <cstddef>
#include <stdexcept>
#include <string>

using test_log::LogCapture;

TEST_CASE("Logger: a message given alone is logged as it is, % signs included")
{
    LogCapture c;

    logger->info("100% done %s %n");
    std::string name = "pass_50%d_%s.cadu";
    logger->warn(name);
    std::string dir = "/data/%x";
    logger->error("Could not open " + dir + "/%n.raw");
    char buf[] = "%s%s%s%s";
    logger->debug(buf);
    const char *p = "%p %%";
    logger->trace(p);
    logger->notice(std::string("%%"));
    std::runtime_error e("bad value %s at %d%%");
    logger->critical(e.what());
    logger->info("ends with %");

    REQUIRE(c.count() == 8);
    CHECK(c.text(0) == "100% done %s %n");
    CHECK(c.text(1) == "pass_50%d_%s.cadu");
    CHECK(c.text(2) == "Could not open /data/%x/%n.raw");
    CHECK(c.text(3) == "%s%s%s%s");
    CHECK(c.text(4) == "%p %%");
    CHECK(c.text(5) == "%%");
    CHECK(c.text(6) == "bad value %s at %d%%");
    CHECK(c.text(7) == "ends with %");

    CHECK(c.sink->msgs[0].lvl == slog::LOG_INFO);
    CHECK(c.sink->msgs[1].lvl == slog::LOG_WARN);
    CHECK(c.sink->msgs[2].lvl == slog::LOG_ERROR);
    CHECK(c.sink->msgs[3].lvl == slog::LOG_DEBUG);
    CHECK(c.sink->msgs[4].lvl == slog::LOG_TRACE);
    CHECK(c.sink->msgs[5].lvl == slog::LOG_NOTICE);
    CHECK(c.sink->msgs[6].lvl == slog::LOG_CRIT);
}

TEST_CASE("Logger: a null message is logged as (null) instead of throwing")
{
    LogCapture c;
    const char *nothing = nullptr;
    CHECK_NOTHROW(logger->error(nothing));
    REQUIRE(c.count() == 1);
    CHECK(c.last() == "(null)");
}

TEST_CASE("Logger: calls with arguments are printf-formatted as before")
{
    LogCapture c;

    logger->info("Frame %d of %d", 3, 10);
    logger->info("%.2f MHz", 137.1);
    logger->info("%s/%s", "a", std::string("b").c_str());
    logger->info("%5.1f%%", 99.94);
    logger->info("Decoded %s: %d lines (%s)", "AVHRR", 1200, "ok");
    logger->info("%llu %zu %c %x %u %ld", 1ULL << 40, (size_t)5, 'z', 255, 7u, -3L);
    logger->warn("%s", "100% done %s %n"); // % in an argument is copied, not read
    logger->info(42);
    logger->error(-7);

    REQUIRE(c.count() == 9);
    CHECK(c.text(0) == "Frame 3 of 10");
    CHECK(c.text(1) == "137.10 MHz");
    CHECK(c.text(2) == "a/b");
    CHECK(c.text(3) == " 99.9%");
    CHECK(c.text(4) == "Decoded AVHRR: 1200 lines (ok)");
    CHECK(c.text(5) == "1099511627776 5 z ff 7 -3");
    CHECK(c.text(6) == "100% done %s %n");
    CHECK(c.text(7) == "42");
    CHECK(c.text(8) == "-7");
    CHECK(c.sink->msgs[6].lvl == slog::LOG_WARN);
    CHECK(c.sink->msgs[8].lvl == slog::LOG_ERROR);
}

TEST_CASE("Logger: run-time text given as a %s argument is logged as it is")
{
    LogCapture c;

    // An AX.25 callsign is decoded from the received frame (each byte shifted right by one),
    // so it can hold any character: 0x4A gives '%' and 0xE6 gives 's'. The AX.25 decoder used
    // to put it into the format of a call with arguments, which crashed on such a frame.
    const unsigned char frm[7] = {0x82, 0x84, 0x4A, 0xE6, 0x86, 0x88, 0x60};
    std::string callsign;
    for (int i = 0; i < 6; i++)
        callsign += (char)(frm[i] >> 1);
    REQUIRE(callsign == "AB%sCD");
    logger->trace("AX.25 Frame - SRC : %s,%d DST : %s,%d CTRL : %d PID : %d", callsign.c_str(), frm[6] >> 1, "%n%n%n", 0, 3, 240);

    // A file name, a URL and a flowgraph node id with % in them, in the forms the fixed call
    // sites use
    std::string path = "/data/pass_%s_%n.json";
    std::string url = "https://tiles.example/%7Bz%7D/%s.png";
    std::string node_id = "node_%d%d%d";
    logger->error("Common instrument configuration (%s) invalid! %s", path.c_str(), "bad %s");
    logger->error("Error downloading tile from: %s %s", url.c_str(), "timeout");
    logger->error("Error adding node with ID : %s, Error : %s", node_id.c_str(), "missing %x");

    REQUIRE(c.count() == 4);
    CHECK(c.text(0) == "AX.25 Frame - SRC : AB%sCD,48 DST : %n%n%n,0 CTRL : 3 PID : 240");
    CHECK(c.text(1) == "Common instrument configuration (/data/pass_%s_%n.json) invalid! bad %s");
    CHECK(c.text(2) == "Error downloading tile from: https://tiles.example/%7Bz%7D/%s.png timeout");
    CHECK(c.text(3) == "Error adding node with ID : node_%d%d%d, Error : missing %x");
}

TEST_CASE("Logger: long messages are logged whole and short ones are not padded")
{
    LogCapture c;

    // Around and well past the 1024-byte first try of the formatter
    for (size_t n : {size_t(1), size_t(1022), size_t(1023), size_t(1024), size_t(1025), size_t(5000)})
    {
        std::string body(n, 'a');
        body.back() = 'z';

        logger->info("%s", body.c_str());
        CHECK(c.last().size() == n);
        CHECK(c.last() == body);

        logger->info(body);
        CHECK(c.last() == body);
    }

    logger->info("%d", 5);
    CHECK(c.last().size() == 1);
}

TEST_CASE("Logger: the console and log-file line keeps the end of a long message")
{
    LogCapture c;

    // The length of the line around a message (time stamp, level, line break)
    logger->info("x");
    REQUIRE(c.count() == 1);
    const size_t frame = c.sink->line(c.sink->msgs[0]).size() - 1;

    // Lines of 1023, 1024 and 1025 bytes (the old formatter's buffer) and a much longer one
    for (size_t line_size : {size_t(1023), size_t(1024), size_t(1025), frame + 3003})
    {
        CAPTURE(line_size);
        std::string msg = std::string(line_size - frame - 3, 'b') + "END";
        logger->info(msg);

        std::string line = c.sink->line(c.sink->msgs.back());
        CHECK(line.size() == line_size);
        CHECK(line.find(msg) != std::string::npos);
        CHECK(line.substr(line.size() - 4) == "END\n");
    }
}

TEST_CASE("Logger: a format that cannot be printed logs the format text, not an empty line")
{
    LogCapture c;
    // An unpaired UTF-16 surrogate cannot be converted to a multibyte string, so
    // vsnprintf returns -1 (EILSEQ)
    logger->warn("bad wide text: %ls", L"\xD800");
    REQUIRE(c.count() == 1);
    CHECK(c.last().find("bad wide text: %ls") != std::string::npos);
    CHECK(c.sink->msgs[0].lvl == slog::LOG_WARN);
}
