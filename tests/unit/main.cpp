#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest.h"
#include "logger.h"

int main(int argc, char **argv)
{
    // Code under test logs; keep the output to warnings and errors
    initLogger();
    logger->set_level(slog::LOG_WARN);

    doctest::Context context(argc, argv);
    return context.run();
}
