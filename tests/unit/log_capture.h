// Collects what the logger sends to its sinks while a LogCapture exists, with nothing
// printed to the console, so a test can check what was logged.
#pragma once

#include "logger.h"
#include <memory>
#include <string>
#include <vector>

namespace test_log
{
    class CaptureSink : public slog::LoggerSink
    {
    public:
        std::vector<slog::LogMsg> msgs;

        void receive(slog::LogMsg log) { msgs.push_back(log); }

        // The line the console and file sinks write for a message
        std::string line(const slog::LogMsg &m) { return format_log(m, false); }
    };

    class LogCapture
    {
    private:
        slog::LogLevel old_level;

    public:
        std::shared_ptr<CaptureSink> sink = std::make_shared<CaptureSink>();

        LogCapture()
        {
            old_level = logger->get_level();
            logger->set_level(slog::LOG_TRACE);
            setConsoleLevel(slog::LOG_OFF);
            logger->add_sink(sink);
            sink->msgs.clear(); // add_sink replays the start-up messages
        }

        ~LogCapture()
        {
            logger->del_sink(sink);
            setConsoleLevel(slog::LOG_TRACE); // what initLogger() sets
            logger->set_level(old_level);
        }

        size_t count() const { return sink->msgs.size(); }
        const std::string &text(size_t i) const { return sink->msgs.at(i).str; }
        const std::string &last() const { return sink->msgs.at(sink->msgs.size() - 1).str; }

        // Whether a message at this level or above contains the text
        bool has(slog::LogLevel min_level, const std::string &part) const
        {
            for (auto &m : sink->msgs)
                if (m.lvl >= min_level && m.str.find(part) != std::string::npos)
                    return true;
            return false;
        }
    };
} // namespace test_log
