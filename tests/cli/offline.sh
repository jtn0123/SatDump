#!/bin/bash
# Fork: SatDump run the way the Controller runs it, from a data folder laid out as
# the receiving station's SatDump folder is (home/, data/, passes/), with settings that name local files
# (file://) for the orbit elements and the IERS data:
#   - nothing is downloaded: the log shows no download, for the products step (satdump pipeline
#     on the synthetic MetOp recording the golden test decodes) and for a short live run
#   - the orbit elements and the IERS data are read from the files when SatDump starts, and the
#     products carry those elements
#   - with the files missing, both still end with exit code 0, with one warning per file, and
#     the data read before is still used
#   - an element file with a value a TLE cannot be made from is refused with one warning, and
#     elements of that kind already in the database are skipped with a warning (both stopped
#     SatDump at start-up, or made it drop the database for the run)
#   - the installed defaults download nothing, with a new database, where any update they
#     scheduled would be due at once
# Where the system allows it (unshare), every run is in a network namespace with no network
# at all, where host names cannot be looked up either. Every run also has its web proxy set to
# a closed port on this machine, so a download that came back by mistake never leaves it (curl
# hands the request to the proxy and does not look the host name up). Where unshare is not
# allowed (in some containers, for one) the checks still run, with the proxy only, and the test
# then ends with exit code 77, which ctest shows as skipped; with SATDUMP_REQUIRE_NETNS=1 that
# is a failure instead.
TESTS=$(cd "$(dirname "$0")/.." && pwd)
. "$TESTS/cli/common.sh"

# Nothing listens on port 9 (discard) here; no_proxy is empty so that no host is left out
for v in http_proxy https_proxy HTTPS_PROXY ALL_PROXY all_proxy; do export "$v=http://127.0.0.1:9"; done
export no_proxy="" NO_PROXY=""

# A network namespace alone still lets a program look up host names: the C library asks the
# system's name service (systemd-resolved, nscd) through a socket in /run, which is outside
# the namespace, and that service queries DNS. So the runs also get a mount namespace where
# names come from /etc/hosts only and those sockets are hidden.
sed 's/^hosts:.*/hosts: files/' /etc/nsswitch.conf > "$WORK/nsswitch.conf" 2> /dev/null ||
    echo 'hosts: files' > "$WORK/nsswitch.conf"
cat > "$WORK/nonet.sh" << 'SH'
mount --bind "$NSS" /etc/nsswitch.conf || exit 125
for d in /run/systemd/resolve /run/nscd /var/run/nscd; do
    if [ -d "$d" ] && ! [ -L "$d" ]; then mount -t tmpfs none "$d" || exit 125; fi
done
exec "$@"
SH
export NSS="$WORK/nsswitch.conf"
NONET=()
if unshare -rmn bash "$WORK/nonet.sh" bash -c '[ "$(grep "^hosts:" /etc/nsswitch.conf)" = "hosts: files" ] &&
    ! [ -e /run/systemd/resolve/io.systemd.Resolve ] && ! ip -o link 2> /dev/null | grep -qv " lo:"' 2> /dev/null; then
    NONET=(unshare -rmn bash "$WORK/nonet.sh")
    echo "ok   no network and no name lookups for the runs below (unshare -rmn)"
elif [ "${SATDUMP_REQUIRE_NETNS:-0}" = 1 ]; then
    echo "FAIL unshare -rmn is not allowed here, and SATDUMP_REQUIRE_NETNS=1 asks for it"
    FAILED=1
else
    echo "note: unshare -rmn is not allowed here: the runs keep the network, with the proxy on a closed local port; downloads are checked from the logs"
fi

# The installed defaults name no source to download from, and switch both updates off
DEFAULTS=$(find "$PREFIX" -name satdump_cfg.json -print -quit)
check defaults-found test -n "$DEFAULTS"
check defaults-no-kepler-download python3 -I -B - "$DEFAULTS" <<'PY'
import json, re, sys
text = open(sys.argv[1]).read()
# The file has // comments; drop them outside strings ("http://..." stays)
cfg = json.loads(re.sub(r'("(?:[^"\\]|\\.)*")|//[^\n]*', lambda m: m.group(1) or "", text))
g = cfg["satdump_general"]
ok = (cfg["kepler_settings"]["urls_to_fetch"] == [] and cfg["kepler_settings"]["norads_to_fetch"] == []
      and g["kepler_update_interval"]["value"] == "Never" and g["iers_auto_update"]["value"] is False
      and g["iers_bulletin_url"]["value"].startswith("https://") and g["leap_seconds_url"]["value"].startswith("https://"))
sys.exit(0 if ok else 1)
PY

# The Controller's folder: HOME and XDG_CONFIG_HOME under home/, its data files under data/
D="$WORK/station/satdump"
mkdir -p "$D/home/.config/satdump" "$D/data" "$D/passes"
export HOME="$D/home" XDG_CONFIG_HOME="$D/home/.config"
cat > "$D/home/.config/satdump/settings.json" << JSON
{
    "satdump_general": {
        "kepler_update_interval": { "value": "4 hours" },
        "iers_auto_update": { "value": true },
        "iers_bulletin_url": { "value": "file://$D/data/finals2000A.daily.json" },
        "leap_seconds_url": { "value": "file://$D/data/leap-seconds.list" }
    },
    "kepler_settings": {
        "urls_to_fetch": [ "file://$D/data/elements.tle" ],
        "norads_to_fetch": []
    }
}
JSON
# Made-up elements in the shape of three weather satellites'
cat > "$D/data/elements.tle" << 'TLE'
METOP-B
1 38771U 12049A   25314.50000000  .00000100  00000-0  50000-4 0  9992
2 38771  98.6900  10.1234 0001500  90.1000 270.0500 14.21512345678909
NOAA 19
1 33591U 09005A   25314.50000000  .00000100  00000-0  50000-4 0  9995
2 33591  99.0500  40.5000 0001400 100.2000 260.0000 14.13000000865436
METEOR-M2 3
1 57166U 23091A   25314.50000000  .00000100  00000-0  50000-4 0  9990
2 57166  98.6500  20.7500 0003000  80.5000 279.6000 14.24000000123455
TLE
cp "$D/data/elements.tle" "$WORK/elements.tle"
cp "$TESTS/offline/finals2000A.daily.json" "$TESTS/offline/leap-seconds.list" "$D/data/"

# run_off <expected exit code> <name> <args...>: like run(), with no network when possible,
# the log at info level (-v), and stopped after 120 s
run_off() {
    local want=$1 name=$2
    shift 2
    timeout -k 5 120 "${NONET[@]}" "$SD" -v "$@" > "$WORK/$name.log" 2>&1
    local got=$?
    if [ "$got" = "$want" ]; then
        echo "ok   $name (exit $got)"
    else
        echo "FAIL $name: exit $got, expected $want"
        sed 's/\x1b\[[0-9;]*m//g' "$WORK/$name.log" | tail -8 | sed 's/^/     /'
        FAILED=1
    fi
}
plain() { sed 's/\x1b\[[0-9;]*m//g' "$WORK/$1.log"; }
logged() { plain "$1" | grep -q "$2"; }
not_logged() { ! logged "$@"; }
count() { plain "$1" | grep -c "$2"; }
# Anything that would have gone online: a download message, a web address, a curl error
no_download() { ! plain "$1" | grep -E -i "Downloading|https?://|curl_easy_perform|Updating Keplers.*http|Retrying"; }
# The files were read, and nothing went wrong with them (the leap second list's warnings all
# start "The leap second list": one that is refused, for its hash line or anything else, shows)
read_files() {
    logged "$1" "Read 3 orbit elements from file://$D/data/elements.tle" &&
        logged "$1" "Reading the IERS bulletin from file://$D/data/finals2000A.daily.json" &&
        logged "$1" "Reading the leap second list from file://$D/data/leap-seconds.list" &&
        logged "$1" "4 IERS Records in database" &&
        ! logged "$1" "Cannot read the" &&
        ! logged "$1" "The leap second list"
}

# The synthetic MetOp recording of the golden test, decoded to products
python3 -I -B "$TESTS/golden/make_recordings.py" "$WORK/in" > /dev/null || exit 1
PASS="$D/passes/20261112T101500Z_38771"
mkdir -p "$PASS/live-1"

run_off 0 products pipeline metop_ahrpt cadu "$WORK/in/metop.cadu" "$PASS/products/live-1"
check products-made test -f "$PASS/products/live-1/dataset.json"
check products-read-files read_files products
check products-no-download no_download products

# The products carry the file's elements, in TLE form, as plain text (the international
# designator was once padded with NUL bytes): NOAA 19's, as the golden NOAA recording is NOAA 19
carries_elements() {
    python3 -I -B -c 'import pathlib, sys
line1 = b"1 33591U 09005A   25314.50000000 "
sys.exit(0 if any(line1 in p.read_bytes() for p in pathlib.Path(sys.argv[1]).rglob("product.cbor")) else 1)' "$1"
}
run_off 0 products-noaa pipeline noaa_hrpt frames "$WORK/in/noaa.raw16" "$WORK/products-noaa"
check products-noaa-carry-file-elements carries_elements "$WORK/products-noaa"
check products-noaa-no-download no_download products-noaa

# A short live run from a recording (the file source plays it in real time)
head -c 8000000 /dev/zero > "$WORK/in.cf32"
LIVE=(legacy live metop_ahrpt "$PASS/live-1" --source file --file_path "$WORK/in.cf32" --baseband_type cf32
    --samplerate 6000000 --frequency 1701300000 --timeout 3)
run_off 0 live "${LIVE[@]}"
check live-read-files read_files live
check live-no-download no_download live

# The data files missing: the runs still end normally, with one warning per file
rm -f "$D/data/elements.tle" "$D/data/finals2000A.daily.json" "$D/data/leap-seconds.list"
run_off 0 products-no-files pipeline metop_ahrpt cadu "$WORK/in/metop.cadu" "$PASS/products-no-files"
check products-no-files-made test -f "$PASS/products-no-files/dataset.json"
for f in "orbit element file file://$D/data/elements.tle" "IERS bulletin file file://$D/data/finals2000A.daily.json" \
    "leap second list file file://$D/data/leap-seconds.list"; do
    check "products-no-files-one-warning ($f)" [ "$(count products-no-files "Cannot read the $f (no such file)")" = 1 ]
done
check products-no-files-no-download no_download products-no-files
# The data read before is kept, and used
check products-no-files-keep-elements logged products-no-files "3 Keplers in database"
check products-no-files-keep-iers logged products-no-files "4 IERS Records in database"
run_off 0 products-noaa-no-files pipeline noaa_hrpt frames "$WORK/in/noaa.raw16" "$WORK/products-noaa-no-files"
check products-noaa-no-files-carry-elements carries_elements "$WORK/products-noaa-no-files"
run_off 0 live-no-files "${LIVE[@]/live-1/live-2}"
check live-no-files-one-warning [ "$(count live-no-files "Cannot read the orbit element file")" = 1 ]
check live-no-files-no-download no_download live-no-files

# An element file whose entries all read, but with a drag term (BSTAR) of 1.2, as a column mix-up
# gives (OMM CSV this time; the format is found from the content): a TLE cannot be made from it.
# It was stored, and making the TLEs then stopped every command at start-up (exit 134). Now the
# file is refused with one warning, and the elements read before are kept
cat > "$D/data/elements.tle" << 'CSV'
OBJECT_NAME,OBJECT_ID,EPOCH,MEAN_MOTION,ECCENTRICITY,INCLINATION,RA_OF_ASC_NODE,ARG_OF_PERICENTER,MEAN_ANOMALY,EPHEMERIS_TYPE,CLASSIFICATION_TYPE,NORAD_CAT_ID,ELEMENT_SET_NO,REV_AT_EPOCH,BSTAR,MEAN_MOTION_DOT,MEAN_MOTION_DDOT
METOP-B,2012-049A,2025-11-11T12:00:00.000000,14.21512345,.00015,98.69,10.1234,90.1,270.05,0,U,38771,999,67890,.00005,.000001,0
NOAA 19,2009-005A,2025-11-11T12:00:00.000000,14.13,.00014,99.05,40.5,100.2,260,0,U,33591,999,86543,1.2,.000001,0
CSV
run_off 0 bad-elements pipeline metop_ahrpt cadu "$WORK/in/metop.cadu" "$PASS/products-bad-elements"
check bad-elements-one-warning [ "$(count bad-elements "has 1 entries that cannot be read")" = 1 ]
check bad-elements-keep-database not_logged bad-elements "keeps its data in memory"
check bad-elements-keep-elements logged bad-elements "3 Keplers in database"

# Elements of that kind already in the database (stored by the version that then stopped): they
# are skipped with a warning, and the database is still used. Making their TLE threw, and the
# run dropped main.db for an empty database in memory
check stored-bad-elements-put python3 -I -B - "$D/home/.config/satdump/main.db" << 'PY'
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
db.execute("INSERT INTO kepler VALUES ('99999_1762862400', 99999, 999, 'FORK BAD', '2025-001A', 1762862400, "
           "98.69, 10.1234, 0.00015, 90.1, 270.05, 14.2, 0.000001, 0, 1.5, 100)")
db.commit()
PY
cp "$WORK/elements.tle" "$D/data/elements.tle"
run_off 0 stored-bad-elements pipeline metop_ahrpt cadu "$WORK/in/metop.cadu" "$PASS/products-stored-bad-elements"
check stored-bad-elements-skipped logged stored-bad-elements "Stored orbit elements are not used: the elements of satellite 99999"
check stored-bad-elements-keep-database not_logged stored-bad-elements "keeps its data in memory"
check stored-bad-elements-read-file logged stored-bad-elements "Read 3 orbit elements from file://$D/data/elements.tle"
check stored-bad-elements-keep-elements logged stored-bad-elements "4 Keplers in database"

# The installed defaults (no settings file): nothing is fetched, nothing is read. Only with no
# network, so that defaults that went wrong could never reach a real server from a test. The
# database goes too: an update is scheduled from the time of the last one stored there, which
# the runs above made a few seconds ago, so one the defaults scheduled by mistake would not be
# due for hours and would never show. With a new database it is due at once
if [ ${#NONET[@]} -gt 0 ]; then
    rm -f "$D/home/.config/satdump/settings.json" "$D/home/.config/satdump/main.db"
    run_off 0 defaults pipeline metop_ahrpt cadu "$WORK/in/metop.cadu" "$WORK/out-defaults"
    check defaults-no-kepler-update not_logged defaults "Updating Keplers"
    check defaults-no-iers-update not_logged defaults "IERS bulletin"
    check defaults-no-update-ran not_logged defaults "in database!"
    check defaults-no-download no_download defaults
fi

# Every check passed, but without the network namespace: shown as skipped, not as passed
[ $FAILED = 0 ] && [ ${#NONET[@]} = 0 ] && exit 77
exit $FAILED
