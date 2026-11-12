# Fork build preset for a Raspberry Pi 5 (Debian 13) weather-satellite station.
#
#   cmake -C cmake/fork/pi.cmake -S . -B build -G Ninja
#
# It works on the Pi itself and on any other arm64 Debian 13 machine (the binaries are
# tuned for the Pi 5's Cortex-A76 cores, so they run on the Pi wherever they are built).
# Every value is a cache default: a -D on the command line still wins.

set(CMAKE_BUILD_TYPE Release CACHE STRING "")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "")
set(SATDUMP_CPU_FLAGS "-mcpu=cortex-a76" CACHE STRING "")

# All satellite decoders and all local radio drivers
set(PLUGINS_ALL ON CACHE BOOL "")
set(BUILD_GUI ON CACHE BOOL "")
set(BUILD_OPENMP ON CACHE BOOL "")
set(PLUGIN_SIMD_NEON ON CACHE BOOL "")

# x86-only speed-ups; nothing to build on arm64
set(PLUGIN_SIMD_SSE41 OFF CACHE BOOL "")
set(PLUGIN_SIMD_AVX2 OFF CACHE BOOL "")

# No network radio servers: satdump_sdr_server listens on every address (port 5656) with
# no password, and the plugin's discovery listens on every address too.
set(PLUGIN_REMOTE_SDR_SUPPORT OFF CACHE BOOL "")

# Installed programs (<prefix>/bin) and plugins (<prefix>/lib/satdump/plugins) find
# libsatdump_core in <prefix>/lib, and plugins find the plugins they build on (such as
# libxrit_support) beside them, so an install works wherever it is unpacked
set(CMAKE_INSTALL_RPATH "$ORIGIN/../lib;$ORIGIN/../..;$ORIGIN" CACHE STRING "")
