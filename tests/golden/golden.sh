#!/bin/bash
# Golden tests: decode the synthetic MetOp and NOAA recordings (make_recordings.py) with the
# installed SatDump and compare every picture with its known-good fingerprint.
#
#   golden.sh <install prefix> [--update | --names-only]
#
# The fingerprints are in expected-<arch>.txt: one line per picture (path, size in bytes,
# SHA-256) plus the satellite each dataset names. --update rewrites that file after a change
# that is meant to alter the pictures (check them by eye first). --names-only compares only
# which pictures are made and how big they are; sanitizer builds use it, since their
# different compiler settings can change the last bit of some calibrated values.
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/../cli/common.sh"
MODE=${2:-}
EXPECTED="$HERE/expected-$(uname -m).txt"

# Pictures as binary PGM/PPM: unlike PNG, the bytes do not depend on the zlib version
cat > "$HOME/.config/satdump/settings.json" <<'JSON'
{ "satdump_general": { "kepler_update_interval": { "value": "Never" }, "iers_auto_update": { "value": false },
                       "image_format": { "value": "pbm" }, "product_format": { "value": "pbm" } } }
JSON

python3 -I -B "$HERE/make_recordings.py" "$WORK/in" || exit 1
run 0 metop pipeline metop_ahrpt cadu "$WORK/in/metop.cadu" "$WORK/metop"
run 0 noaa pipeline noaa_hrpt frames "$WORK/in/noaa.raw16" "$WORK/noaa"
[ "$FAILED" = 0 ] || exit 1

python3 -I -B - "$WORK" > "$WORK/got.txt" <<'PY' || exit 1
import hashlib, json, os, sys
work = sys.argv[1]
for run in ("metop", "noaa"):
    top = os.path.join(work, run)
    with open(os.path.join(top, "dataset.json")) as f:
        print(f"{run} satellite {json.load(f).get('satellite')}")
    for d, _, files in sorted(os.walk(top)):
        for name in sorted(files):
            if name.endswith((".pgm", ".ppm", ".pbm")):
                p = os.path.join(d, name)
                with open(p, "rb") as f:
                    data = f.read()
                print(f"{os.path.relpath(p, work)} {len(data)} {hashlib.sha256(data).hexdigest()}")
PY

if [ "$MODE" = --update ]; then
    cp "$WORK/got.txt" "$EXPECTED"
    echo "golden: wrote $(grep -c . "$EXPECTED") lines to $EXPECTED"
    exit 0
fi
[ -f "$EXPECTED" ] || { echo "golden: no $EXPECTED (run with --update on a trusted build)"; exit 1; }

if [ "$MODE" = --names-only ]; then
    cut -d' ' -f1-2 "$EXPECTED" > "$WORK/want.cmp"
    cut -d' ' -f1-2 "$WORK/got.txt" > "$WORK/got.cmp"
else
    cp "$EXPECTED" "$WORK/want.cmp"
    cp "$WORK/got.txt" "$WORK/got.cmp"
fi
if diff -u "$WORK/want.cmp" "$WORK/got.cmp" > "$WORK/diff.txt"; then
    echo "ok   golden: $(grep -c . "$WORK/got.txt") lines match"
    exit 0
fi
echo "FAIL golden: the pictures differ from $EXPECTED"
head -40 "$WORK/diff.txt" | sed 's/^/     /'
exit 1
