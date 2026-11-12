// Wrong printf-style logger calls. tests/compile/logger_format.sh checks that the compiler
// reports every line marked BAD.
#include "logger.h"
#include <filesystem>
#include <string>

void bad_logger_calls(const std::string &s, double d, int i, const std::filesystem::path &path)
{
    // A format built at run time, with arguments: text such as a file name would be read as
    // a format (use "...%s...", s.c_str() instead)
    logger->error("Error: " + s + " %d", i);            // BAD: a concatenated format with an argument
    logger->warn(s, i);                                 // BAD: a std::string format with an argument
    logger->info(std::string("Got %d"), i);             // BAD: a std::string format with an argument
    logger->trace("Frame " + s + " %d %d", i, i);       // BAD: a concatenated format with arguments
    logger->debug(path, d);                             // BAD: a path as a format with an argument

    // Arguments that do not match a string-literal format
    logger->info("%d", d);                  // BAD: a double for %d
    logger->warn("%s", i);                  // BAD: an int for %s
    logger->error("%s and %s", "only one"); // BAD: a missing argument
    logger->debug("%d", i, i);              // BAD: one argument too many
    logger->trace("%s", s);                 // BAD: a std::string for %s
    logger->critical("%f", i);              // BAD: an int for %f
    logger->notice("%zu", s.c_str());       // BAD: a pointer for %zu
}
