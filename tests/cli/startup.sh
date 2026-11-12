#!/bin/bash
# Start-up problems that must not stop SatDump (or that must give the right exit code):
#   - a damaged settings file: SatDump uses the default settings and leaves the file as it is
#   - a damaged database (main.db): this run keeps its data in memory, the file is untouched
#   - HOME not set (services, cron): the settings come from XDG_CONFIG_HOME instead
#   - a radio that is not there: exit code 3 ("radio problem") for record and live
. "$(dirname "$0")/common.sh"

CFG="$HOME/.config/satdump"
DECODE=(pipeline metop_ahrpt cadu "$WORK/in.cadu" "$WORK/out")
# A short input that decodes to nothing; the run still has to start and finish normally
head -c 102400 /dev/zero > "$WORK/in.cadu"

# When the user's settings file cannot be read the defaults apply. The fork's defaults no
# longer download orbit or Earth-rotation data (cli/offline.sh checks that); this test still
# supplies its own defaults with both updates off, so it never depends on them (a
# satdump_cfg.json in the working folder is used instead of the installed one).
DEFAULTS=$(find "$PREFIX" -name satdump_cfg.json -print -quit)
[ -n "$DEFAULTS" ] || { echo "FAIL no satdump_cfg.json under $PREFIX"; exit 1; }
awk '/"kepler_update_interval"/ { k = 1 } /"iers_auto_update"/ { i = 1 }
     k && /"value"/ { sub(/"value": *"[^"]*"/, "\"value\": \"Never\""); k = 0 }
     i && /"value"/ { sub(/"value": *true/, "\"value\": false"); i = 0 }
     { print }' "$DEFAULTS" > "$WORK/satdump_cfg.json"
check defaults-without-downloads grep -q '"value": "Never"' "$WORK/satdump_cfg.json"

# 1. Damaged settings file
printf '{ "satdump_general": { "image_format": ' > "$CFG/settings.json"
cp "$CFG/settings.json" "$WORK/settings.before"
run 0 damaged-settings "${DECODE[@]}"
check damaged-settings-reported grep -q "Cannot read the settings file" "$WORK/damaged-settings.log"
check damaged-settings-unchanged cmp -s "$CFG/settings.json" "$WORK/settings.before"
rm -f "$CFG/settings.json"

# 2. Damaged database
head -c 4096 /dev/urandom > "$CFG/main.db"
cp "$CFG/main.db" "$WORK/main.before"
run 0 damaged-database "${DECODE[@]}"
check damaged-database-reported grep -q "keeps its data in memory" "$WORK/damaged-database.log"
check damaged-database-unchanged cmp -s "$CFG/main.db" "$WORK/main.before"
rm -f "$CFG/main.db"

# 3. HOME not set: settings and database go under XDG_CONFIG_HOME
XDG="$WORK/xdg"
mkdir -p "$XDG"
env -u HOME XDG_CONFIG_HOME="$XDG" "$SD" "${DECODE[@]}" > "$WORK/no-home.log" 2>&1
got=$?
if [ "$got" = 0 ]; then echo "ok   no-home (exit 0)"; else echo "FAIL no-home: exit $got, expected 0"; tail -8 "$WORK/no-home.log" | sed 's/^/     /'; FAILED=1; fi
check no-home-uses-xdg test -f "$XDG/satdump/main.db"

# 4. No such radio: exit code 3, for recording and for live decoding
run 3 record-no-radio legacy record "$WORK/rec" --source no_such_radio --samplerate 2400000 --frequency 137100000 --baseband_format cs16 --timeout 1
run 3 live-no-radio legacy live metop_ahrpt "$WORK/live" --source no_such_radio --samplerate 6000000 --frequency 1701300000 --timeout 1

exit $FAILED
