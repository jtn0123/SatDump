#!/bin/bash
# A relocatable install (CMAKE_INSTALL_RPATH set, as in cmake/fork/pi.cmake) runs with no
# LD_LIBRARY_PATH: every program, library and plugin finds what it links against, and
# satdump finds its resources and plugins next to itself.
. "$(dirname "$0")/common.sh"
unset LD_LIBRARY_PATH

checked=0
while IFS= read -r -d '' f; do
    head -c 4 "$f" | grep -q $'\x7fELF' || continue
    checked=$((checked + 1))
    missing=$(ldd "$f" 2>&1 | grep "not found")
    if [ -n "$missing" ]; then
        echo "FAIL ${f#"$PREFIX"/} cannot find:"
        echo "$missing" | sed 's/^\s*/     /'
        FAILED=1
    fi
done < <(find -L "$PREFIX/bin" "$PREFIX/lib" -type f -print0)
echo "ok   $checked programs and libraries checked"

# A run from a folder that is not the install prefix still loads the MetOp plugin and the
# pipeline list from <prefix>/share/satdump (the cadu level needs both)
: > "$WORK/empty.cadu"
run 0 relocated-run pipeline metop_ahrpt cadu "$WORK/empty.cadu" "$WORK/out"

exit $FAILED
