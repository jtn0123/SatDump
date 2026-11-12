#pragma once

// Fork: muparser comes from the system on Linux (Debian's libmuparser-dev; Android is not
// Linux here) and from the copy in libs/muparser everywhere else (Windows, macOS, Android).
// cmake/fork/system_parsers.cmake makes the same choice, so keep the two conditions alike.
// Off Linux the bundled header is named by its path, so another muparser on the include path
// (Homebrew's, for example) is never mixed with the bundled code. Include muparser through
// this file only.
#if defined(__linux__) && !defined(__ANDROID__)
#define SATDUMP_SYSTEM_MUPARSER 1
#include <muParser.h>
#else
#include "libs/muparser/muParser.h"
#endif
