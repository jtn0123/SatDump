// Every way the code base calls the logger. tests/compile/logger_format.sh compiles it with
// -Wall -Wextra -Werror: no call may be ambiguous or warn.
#include "logger.h"
#include <cstddef>
#include <exception>
#include <filesystem>
#include <string>

enum OldStyleEnum
{
    OLD_STYLE_VALUE = 3,
};

void logger_calls(const std::string &s, const char *p, char *buf, int i, unsigned u, size_t z, long long ll, double d, float f, bool b,
                  const std::exception &e, const std::filesystem::path &path)
{
    // A message alone is logged as it is
    logger->trace("literal");
    logger->debug(s);
    logger->info(p);
    logger->notice(buf);
    logger->warn("prefix " + s + " suffix");
    logger->error(e.what());
    logger->critical(std::string("x") + p);
    logger->info(path.string());
    logger->info(path);
    logger->info(std::string());

    // A number alone (shown as an int, as before)
    logger->info(i);
    logger->info(u);
    logger->info(z);
    logger->info(d);
    logger->info(b);
    logger->info(OLD_STYLE_VALUE);

    // printf formats; run-time text goes in as a %s argument
    logger->info("%d %u %zu %lld %f %f %s %s %c %d", i, u, z, ll, d, f, p, s.c_str(), 'x', OLD_STYLE_VALUE);
    logger->error("Error: %s %d", s.c_str(), i);
    logger->critical(p, d); // a const char * format is accepted, but not checked
    logger->notice("%s", buf);

    // The lower-level calls
    logger->log(slog::LOG_INFO, s);
    logger->log(slog::LOG_INFO, "literal");
    logger->log(slog::LOG_INFO, i);
}
