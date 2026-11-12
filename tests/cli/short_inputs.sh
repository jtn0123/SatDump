#!/bin/bash
# Frame files that are empty, shorter than one frame, or end part-way through a frame must
# finish cleanly: no crash, and no stale or uninitialised bytes treated as a frame.
. "$(dirname "$0")/common.sh"

make_file() { head -c "$2" /dev/urandom > "$WORK/$1"; }
make_file empty.cadu 0
make_file short.cadu 1023
make_file partial.cadu $((3 * 1024 + 500))

for f in empty short partial; do
    run 0 "metop-$f" pipeline metop_ahrpt cadu "$WORK/$f.cadu" "$WORK/metop-$f"
    run 0 "meteor-$f" pipeline meteor_m2-x_lrpt cadu "$WORK/$f.cadu" "$WORK/meteor-$f"
done

exit $FAILED
