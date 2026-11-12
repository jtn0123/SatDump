#pragma once
// Shared set-up for the fuzz harnesses: SatDump code logs through a global logger, which
// must exist. Only critical messages are shown, so the fuzzer's own output stays readable.
#include "logger.h"

extern "C" int LLVMFuzzerInitialize(int *, char ***)
{
    initLogger();
    logger->set_level(slog::LOG_CRIT);
    return 0;
}
