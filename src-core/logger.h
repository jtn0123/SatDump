#pragma once

#include <memory>
#include "dll_export.h"
#include <vector>
#include <fstream>
#include <mutex>
#include <stdarg.h>
#include <string>
#include <type_traits>

// Fork: lets GCC and clang check printf-style logger calls against their arguments (-Wformat)
#if defined(__GNUC__) || defined(__clang__)
#define SLOG_PRINTF_FORMAT(fmt_index, first_arg) __attribute__((format(printf, fmt_index, first_arg)))
#else
#define SLOG_PRINTF_FORMAT(fmt_index, first_arg)
#endif

namespace slog
{
    // Logger log level
    enum LogLevel
    {
        LOG_TRACE,
        LOG_DEBUG,
        LOG_INFO,
        LOG_NOTICE,
        LOG_WARN,
        LOG_ERROR,
        LOG_CRIT,
        LOG_OFF,
    };

    // Struct holding a formatted message
    // Including special color fomatting
    // chars
    struct LogMsg
    {
        std::string str;
        LogLevel lvl;
    };

    // Logger sink
    class LoggerSink
    {
    protected:
        LogLevel sink_lvl = LOG_TRACE;

        std::string format_log(LogMsg m, bool color, int *cpos = nullptr);

    public:
        virtual void receive(LogMsg log) = 0;
        void set_level(LogLevel lvl) { sink_lvl = lvl; }
    };

    // Fork: picks the deleted overloads below for a format given as a std::string (or anything
    // that converts to one) with arguments, never for a C string
    template <typename S>
    using if_string_format = typename std::enable_if<std::is_convertible<const S &, std::string>::value && !std::is_convertible<const S &, const char *>::value>::type;

    class Logger
    {
    private:
        LogLevel logger_lvl = LOG_TRACE;
        std::mutex sink_mtx;
        std::vector<std::shared_ptr<LoggerSink>> sinks;

        // A message given alone, logged as it is; a null pointer logs "(null)"
        void log_text(LogLevel lvl, const char *msg) { log(lvl, msg != nullptr ? std::string(msg) : std::string("(null)")); }

    public:
        void set_level(LogLevel lvl);
        LogLevel get_level();
        void add_sink(std::shared_ptr<LoggerSink> sink);
        void del_sink(std::shared_ptr<LoggerSink> sink);

    public:
        // Fork: a message given alone (a const char *, a std::string or a concatenation) is
        // logged exactly as it is. These used to be printf-style functions only, so a % in a
        // message (a file name, "100%", an exception text) was read as a format: undefined
        // behaviour that could crash. A call with arguments still uses a printf format, and
        // the compiler checks the arguments against a string-literal format (-Wformat).
        // A format with arguments that is a std::string or a concatenation is refused at
        // compile time (the deleted overloads): it mixed run-time text (a file name, a
        // decoded callsign) into the format, with the same crash. Pass such text as a %s
        // argument instead: logger->info("Saved %s (%d)", path.c_str(), n).
        // `template <typename = void>` only makes a call with one argument pick the plain
        // overload.
        void trace(const char *msg) { log_text(LOG_TRACE, msg); }
        void trace(const std::string &msg) { log(LOG_TRACE, msg); }
        template <typename = void>
        SLOG_PRINTF_FORMAT(2, 3) void trace(const char *fmt, ...)
        {
            va_list args;
            va_start(args, fmt);
            logf(LOG_TRACE, fmt, args);
            va_end(args);
        }
        template <typename S, typename A, typename... Args, typename = if_string_format<S>>
        void trace(const S &fmt, A a, Args... args) = delete;

        void debug(const char *msg) { log_text(LOG_DEBUG, msg); }
        void debug(const std::string &msg) { log(LOG_DEBUG, msg); }
        template <typename = void>
        SLOG_PRINTF_FORMAT(2, 3) void debug(const char *fmt, ...)
        {
            va_list args;
            va_start(args, fmt);
            logf(LOG_DEBUG, fmt, args);
            va_end(args);
        }
        template <typename S, typename A, typename... Args, typename = if_string_format<S>>
        void debug(const S &fmt, A a, Args... args) = delete;

        void info(const char *msg) { log_text(LOG_INFO, msg); }
        void info(const std::string &msg) { log(LOG_INFO, msg); }
        template <typename = void>
        SLOG_PRINTF_FORMAT(2, 3) void info(const char *fmt, ...)
        {
            va_list args;
            va_start(args, fmt);
            logf(LOG_INFO, fmt, args);
            va_end(args);
        }
        template <typename S, typename A, typename... Args, typename = if_string_format<S>>
        void info(const S &fmt, A a, Args... args) = delete;

        void notice(const char *msg) { log_text(LOG_NOTICE, msg); }
        void notice(const std::string &msg) { log(LOG_NOTICE, msg); }
        template <typename = void>
        SLOG_PRINTF_FORMAT(2, 3) void notice(const char *fmt, ...)
        {
            va_list args;
            va_start(args, fmt);
            logf(LOG_NOTICE, fmt, args);
            va_end(args);
        }
        template <typename S, typename A, typename... Args, typename = if_string_format<S>>
        void notice(const S &fmt, A a, Args... args) = delete;

        void warn(const char *msg) { log_text(LOG_WARN, msg); }
        void warn(const std::string &msg) { log(LOG_WARN, msg); }
        template <typename = void>
        SLOG_PRINTF_FORMAT(2, 3) void warn(const char *fmt, ...)
        {
            va_list args;
            va_start(args, fmt);
            logf(LOG_WARN, fmt, args);
            va_end(args);
        }
        template <typename S, typename A, typename... Args, typename = if_string_format<S>>
        void warn(const S &fmt, A a, Args... args) = delete;

        void error(const char *msg) { log_text(LOG_ERROR, msg); }
        void error(const std::string &msg) { log(LOG_ERROR, msg); }
        template <typename = void>
        SLOG_PRINTF_FORMAT(2, 3) void error(const char *fmt, ...)
        {
            va_list args;
            va_start(args, fmt);
            logf(LOG_ERROR, fmt, args);
            va_end(args);
        }
        template <typename S, typename A, typename... Args, typename = if_string_format<S>>
        void error(const S &fmt, A a, Args... args) = delete;

        void critical(const char *msg) { log_text(LOG_CRIT, msg); }
        void critical(const std::string &msg) { log(LOG_CRIT, msg); }
        template <typename = void>
        SLOG_PRINTF_FORMAT(2, 3) void critical(const char *fmt, ...)
        {
            va_list args;
            va_start(args, fmt);
            logf(LOG_CRIT, fmt, args);
            va_end(args);
        }
        template <typename S, typename A, typename... Args, typename = if_string_format<S>>
        void critical(const S &fmt, A a, Args... args) = delete;

        // Int formats
        void trace(int v) { log(LOG_TRACE, v); }
        void debug(int v) { log(LOG_DEBUG, v); }
        void info(int v) { log(LOG_INFO, v); }
        void notice(int v) { log(LOG_NOTICE, v); } // Fork: was logged at info level
        void warn(int v) { log(LOG_WARN, v); }
        void error(int v) { log(LOG_ERROR, v); }
        void critical(int v) { log(LOG_CRIT, v); }

        SLOG_PRINTF_FORMAT(3, 0) void logf(LogLevel lvl, const char *fmt, va_list args);
        void log(LogLevel lvl, int v) { log(lvl, std::to_string(v)); }
        void log(LogLevel lvl, const std::string &v);
    };

    class StdOutSink : public LoggerSink
    {
    public:
        void receive(LogMsg log);
    };

#if defined(_WIN32)
    class WinOutSink : public LoggerSink
    {
    public:
        void receive(LogMsg log);
    };
#endif

#ifdef __ANDROID__
    class AndroidSink : public LoggerSink
    {
    public:
        void receive(LogMsg log);
    };
#endif

    class FileSink : public LoggerSink
    {
    private:
        std::ofstream outf;

    public:
        FileSink(std::string path);
        ~FileSink();
        void receive(LogMsg log);
    };
}

// Main logger instance
SATDUMP_DLL extern std::shared_ptr<slog::Logger> logger;

// Initialize the logger
void initLogger();

// Init logger file sink
void initFileSink();

// Tell the logger that initialization is done so it stops buffering messages
void completeLoggerInit();

// Change output level, eg Debug, Info, etc
void setConsoleLevel(slog::LogLevel level);