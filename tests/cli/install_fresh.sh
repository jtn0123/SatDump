#!/bin/bash
# The install prefix holds only what the last install wrote ($2 is the build folder's
# install_manifest.txt, cmake's list of it). The cli tests use any file an earlier install left
# there: common.sh puts <prefix>/lib first in LD_LIBRARY_PATH, so a libsatdump_core left there
# by an install with another libdir was run instead of this build's, and other tests take the
# first libsatdump_core.so or satdump_cfg.json they find. tests/gate.sh empties a prefix inside
# the build folder before it installs; a prefix elsewhere has to be emptied by hand.
. "$(dirname "$0")/common.sh"
MANIFEST=${2:?usage: $0 <install prefix> <install_manifest.txt>}
[ -s "$MANIFEST" ] || { echo "FAIL no list of installed files at $MANIFEST (install first, as tests/gate.sh does)"; exit 1; }

export LC_ALL=C
# cmake writes some of the paths with "/./" in them
sed 's|/\./|/|g' "$MANIFEST" | sort -u > "$WORK/installed.txt"
find "$PREFIX" \( -type f -o -type l \) | sort > "$WORK/present.txt"
comm -23 "$WORK/present.txt" "$WORK/installed.txt" > "$WORK/left.txt"
check only-files-of-the-last-install test ! -s "$WORK/left.txt"
if [ -s "$WORK/left.txt" ]; then
    echo "     $(wc -l < "$WORK/left.txt") files under $PREFIX were not written by the last install, such as:"
    head -5 "$WORK/left.txt" | while IFS= read -r f; do echo "     ${f#"$PREFIX"/}"; done
fi

exit $FAILED
