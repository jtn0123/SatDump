#!/bin/bash
# Fork: the installed headers leave out code satdump_core does not build (cmake/fork/codecs.cmake):
# muparserx everywhere, and on Linux the bundled bzip2, OpenJPEG and libaec, whose functions the
# installed library does not have (it uses the system's). Reads the list of files the last
# install wrote, as an install over an older one leaves the older files in the folder.
#   install_headers.sh <build dir>
set -u
M=${1:?usage: $0 <build dir>}/install_manifest.txt
[ -f "$M" ] || { echo "no $M (run tests/gate.sh, which installs first)"; exit 1; }
FAILED=0

left_out="muparserx"
[ "$(uname -s)" = Linux ] && left_out="$left_out bzlib openjp2 aec"
for d in $left_out; do
    n=$(grep -cE "/include/satdump/(\./)*libs/$d/" "$M")
    if [ "$n" = 0 ]; then
        echo "ok   no headers from libs/$d"
    else
        echo "FAIL $n headers from libs/$d are installed, but satdump_core does not build it"
        FAILED=1
    fi
done

# The core's own headers next to them are still installed
for h in libs/bzlib_utils.h image/j2k_stream.h; do
    if grep -qE "/include/satdump/(\./)*$h\$" "$M"; then
        echo "ok   $h installed"
    else
        echo "FAIL $h is not installed"
        FAILED=1
    fi
done

exit $FAILED
