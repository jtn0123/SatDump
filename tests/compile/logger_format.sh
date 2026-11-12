#!/bin/bash
# Compile check of the logger calls (src-core/logger.h).
#   logger_format.sh <c++ compiler> <src-core folder>
# logger_ok.cpp, every way the code base calls the logger, must compile with no warning.
# logger_bad.cpp must get a diagnostic on each line marked BAD: printf-style calls with
# arguments are checked by the compiler (-Wformat), as printf itself is, and a format built at
# run time (a std::string, a concatenation) with arguments does not compile.
set -uo pipefail
CXX=${1:?compiler}
CORE=${2:?src-core folder}
HERE=$(cd "$(dirname "$0")" && pwd)
FLAGS=(-std=c++17 -fsyntax-only -Wall -Wextra -I"$CORE")
FAILED=0

if ! out=$("$CXX" "${FLAGS[@]}" -Werror "$HERE/logger_ok.cpp" 2>&1); then
    echo "$out" | head -40
    echo "FAIL: logger_ok.cpp does not compile cleanly"
    FAILED=1
else
    echo "ok: logger_ok.cpp compiles with no warning"
fi

out=$("$CXX" "${FLAGS[@]}" "$HERE/logger_bad.cpp" 2>&1)
for line in $(grep -n 'BAD:' "$HERE/logger_bad.cpp" | cut -d: -f1); do
    if echo "$out" | grep -qE "logger_bad\.cpp:$line:[0-9]+: (warning|error)"; then
        echo "ok: line $line of logger_bad.cpp is reported"
    else
        echo "FAIL: line $line of logger_bad.cpp ($(sed -n "${line}p" "$HERE/logger_bad.cpp" | sed 's/^ *//')) is not reported"
        FAILED=1
    fi
done
exit $FAILED
