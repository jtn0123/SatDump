#!/bin/bash
# Fork: SatDump started from a folder it cannot read, as with "sudo -u someuser satdump ..."
# run from another user's home (mode 0700). At start-up SatDump looks in the current folder for
# a portable copy (resources, plugins, satdump_cfg.json, settings.json, pipelines.json); those
# checks threw on "Permission denied", so every command stopped at once with "SatDump stopped on
# an error that was not handled" (exit 134). A path there that cannot be checked now counts as
# missing, so the installed files are used. Checked here, each time without an unhandled error:
#   - a current folder that cannot be read or searched (mode 000): the version, the start-up
#     log (installed config, one debug line), and a short live run from a file, which writes
#     its output
#   - a current folder that has been deleted (making ./plugins absolute threw there), and, as
#     a smoke check only, a current folder below a folder that cannot be searched
#   - HOME inside a folder that cannot be read (su without -): the default settings are used
#   - a "plugins" folder in the current folder that cannot be read, or only listed: the
#     installed plugins are loaded
#   - a "resources" folder in the current folder, or a "pipelines" folder in it, that cannot
#     be read: the same as an empty "resources" folder (exit 1, no pipelines), not an
#     unhandled error; a "pipelines" folder that can only be listed: its files are skipped
#     with a warning; a pipeline that includes a file through a folder that cannot be
#     searched: reported like a missing include
#   - a readable current folder with satdump_cfg.json and settings.json: both still used
# Root can read a folder with mode 000. So as root (CI containers) SatDump runs as user nobody
# (setpriv or runuser) or, failing that, without capabilities (unshare -U). When none of these
# works, the runs that need an unreadable folder are not a real check: the test says so and
# ends with exit code 77, which ctest shows as skipped; with SATDUMP_REQUIRE_UNREADABLE=1 (set in
# CI) that is a failure instead, so the check cannot quietly stop running there.
. "$(dirname "$0")/common.sh"
# Folders left with mode 000 could not be removed
trap 'chmod -R u+rwX "$WORK" 2> /dev/null; rm -rf "$WORK"' EXIT

OUT="$WORK/out"
mkdir -p "$OUT"
head -c 8000000 /dev/zero > "$WORK/in.cf32"
LIVE=(legacy live metop_ahrpt "$OUT/live" --source file --file_path "$WORK/in.cf32" --baseband_type cf32
    --samplerate 6000000 --frequency 1701300000 --timeout 2)

# The folder setups, run in the folder SatDump starts from, just before it starts
lock_here() { chmod 000 .; }
lock_parent() { chmod 000 ..; }
remove_here() { rmdir "$PWD"; }
lock_home() {
    chmod 000 "$WORK/lockedhome"
    export HOME="$WORK/lockedhome/home"
}

# run_in <expected exit code> <name> <folder> <setup> <args...>: like run(), but SatDump runs
# from <folder>, as the user in AS, after the setup function <setup> has run there
AS=()
run_in() {
    local want=$1 name=$2 dir=$3 setup=$4
    shift 4
    (cd "$dir" && "$setup" && exec "${AS[@]}" "$SD" "$@") > "$WORK/$name.log" 2>&1
    local got=$?
    if [ "$got" = "$want" ]; then
        echo "ok   $name (exit $got)"
    else
        echo "FAIL $name: exit $got, expected $want"
        sed 's/\x1b\[[0-9;]*m//g' "$WORK/$name.log" | tail -8 | sed 's/^/     /'
        FAILED=1
    fi
}
logged() { sed 's/\x1b\[[0-9;]*m//g' "$WORK/$1.log" | grep -q -- "$2"; }
not_logged() { ! logged "$@"; }
count() { sed 's/\x1b\[[0-9;]*m//g' "$WORK/$1.log" | grep -c -- "$2"; }

# check_denied <name> <command...>: a check of what SatDump reports about a folder it cannot
# read; it only holds where the folder really cannot be read
check_denied() {
    if [ $EXERCISED = 1 ]; then check "$@"; else echo "skip $1 (not exercised, see above)"; fi
}

# blocked: whether the user in AS really cannot list a folder with mode 000 it is in
blocked() {
    mkdir -p "$WORK/probe"
    (cd "$WORK/probe" && chmod 000 . && ! "${AS[@]}" ls . > /dev/null 2>&1)
    local r=$?
    chmod 755 "$WORK/probe"
    return $r
}

EXERCISED=1
if [ "$(id -u)" = 0 ]; then
    # Another user needs to pass through the test folder and write the outputs and settings
    chmod 711 "$WORK"
    chmod -R a+rwX "$HOME" "$OUT"
    NOBODY_UID=$(id -u nobody 2> /dev/null)
    NOBODY_GID=$(id -g nobody 2> /dev/null)
    CANDIDATES=()
    if [ -n "$NOBODY_UID" ] && [ "$NOBODY_UID" != 0 ]; then
        command -v setpriv > /dev/null && CANDIDATES+=("setpriv --reuid=$NOBODY_UID --regid=$NOBODY_GID --clear-groups")
        command -v runuser > /dev/null && CANDIDATES+=("runuser -m -u nobody --")
    fi
    command -v unshare > /dev/null && CANDIDATES+=("unshare -U")
    found=""
    for c in "${CANDIDATES[@]}"; do
        read -r -a AS <<< "$c"
        # That user must be able to run SatDump from a folder it can read, and not list one with mode 000
        if (cd "$WORK" && "${AS[@]}" "$SD" legacy version > "$WORK/identity.log" 2>&1) && blocked; then
            found=$c
            break
        fi
    done
    if [ -n "$found" ]; then
        echo "ok   running as root: SatDump runs through \"$found\", which cannot read a folder with mode 000"
    else
        AS=()
        EXERCISED=0
        echo "NOT EXERCISED: running as root, which can read a folder with mode 000, and SatDump could not be run as user nobody (setpriv, runuser) or without capabilities (unshare -U); the runs below do not show that an unreadable folder is handled"
        [ -f "$WORK/identity.log" ] && sed 's/\x1b\[[0-9;]*m//g' "$WORK/identity.log" | tail -5 | sed 's/^/     /'
    fi
elif ! blocked; then
    EXERCISED=0
    echo "NOT EXERCISED: this user can list a folder with mode 000 (it has capabilities that override file permissions); the runs below do not show that an unreadable folder is handled"
fi

# 1. The current folder cannot be read or searched
mkdir "$WORK/locked"
run_in 0 locked-version "$WORK/locked" lock_here -v legacy version
chmod 755 "$WORK/locked"
check locked-version-printed logged locked-version "This is SatDump v"
check locked-version-installed-config logged locked-version "Loading config /.*/share/satdump/satdump_cfg.json"
check_denied locked-version-one-debug-line [ "$(count locked-version "in the current folder (Permission denied)")" = 1 ]
# The command of the report: this SatDump has no "version" command, so it is a usage error
# (exit 1), the same as from any folder
run_in 1 locked-version-usage "$WORK/locked" lock_here version
chmod 755 "$WORK/locked"
check locked-version-usage-reported logged locked-version-usage "A subcommand is required"
run_in 0 locked-live "$WORK/locked" lock_here "${LIVE[@]}"
chmod 755 "$WORK/locked"
check locked-live-output test -f "$OUT/live/metop_ahrpt.cadu"

# 2. A folder above the current folder cannot be searched; the current folder is deleted.
# locked-parent is only a smoke check: it passes without the fix as well. On Linux a path
# relative to the current folder never looks at the folders above it, getcwd() works without
# search permission on them, and SatDump resolves no path through them at start-up. The case
# that matters, a current folder that cannot be searched, is checked in 1
mkdir -p "$WORK/parent/sub"
run_in 0 locked-parent "$WORK/parent/sub" lock_parent legacy version
chmod 755 "$WORK/parent"
mkdir "$WORK/gone"
run_in 0 deleted-folder "$WORK/gone" remove_here legacy version

# 3. HOME inside a folder that cannot be read: the default settings, and the database in memory
mkdir -p "$WORK/lockedhome/home/.config/satdump"
cp "$HOME/.config/satdump/settings.json" "$WORK/lockedhome/home/.config/satdump/"
run_in 0 locked-home "$WORK" lock_home legacy version
chmod 755 "$WORK/lockedhome"
check_denied locked-home-settings-reported logged locked-home "Cannot check for the settings file $WORK/lockedhome/home/.config/satdump/settings.json (Permission denied)"
check_denied locked-home-pipelines-reported logged locked-home "Cannot check for user pipelines"

# 4. A "plugins" folder in the current folder that cannot be read, or only listed (one file in
# it that cannot be checked): skipped, and the installed plugins are loaded
mkdir -p "$WORK/plugins-locked/plugins" "$WORK/plugins-listed/plugins"
: > "$WORK/plugins-listed/plugins/libnot_a_plugin.so"
chmod 000 "$WORK/plugins-locked/plugins"
chmod 444 "$WORK/plugins-listed/plugins"
run_in 0 plugins-locked "$WORK/plugins-locked" true legacy version
run_in 0 plugins-listed "$WORK/plugins-listed" true legacy version
chmod 755 "$WORK/plugins-locked/plugins" "$WORK/plugins-listed/plugins"
check_denied plugins-locked-reported logged plugins-locked "Cannot read the plugin folder ./plugins (Permission denied)"
for n in plugins-locked plugins-listed; do
    check "$n-installed-plugins" not_logged "$n" "No Plugins loaded"
done

# 5. A "resources" folder in the current folder that cannot be read: as an empty one, a portable
# copy without pipelines (exit 1), not an unhandled error
mkdir -p "$WORK/resources-locked/resources" "$WORK/resources-empty/resources"
chmod 000 "$WORK/resources-locked/resources"
run_in 1 resources-locked "$WORK/resources-locked" true legacy version
chmod 755 "$WORK/resources-locked/resources"
run_in 1 resources-empty "$WORK/resources-empty" true legacy version
check_denied resources-locked-reported logged resources-locked "Couldn't load pipelines! Was trying : resources/pipelines (Permission denied)"
check resources-empty-reported logged resources-empty "Couldn't load pipelines! Was trying : resources/pipelines"

# A "pipelines" folder in a readable "resources" folder that cannot be read: it can still be
# checked, but not listed (the listing threw), so it ends the same way (where it can be read
# after all, it is an empty folder, which SatDump runs with: exit 0). One that can only be
# listed: its files cannot be checked (that threw), so they are skipped with a warning and
# SatDump runs without them. A pipeline including a file through a folder that cannot be
# searched (that check threw): reported, and the other pipelines load
mkdir -p "$WORK/pipelines-locked/resources/pipelines" "$WORK/pipelines-listed/resources/pipelines" \
    "$WORK/include-locked/resources/pipelines" "$WORK/include-locked/resources/locked"
echo '{}' > "$WORK/pipelines-listed/resources/pipelines/listed.json"
echo '{ "include_test": "../locked/part.json.inc" }' > "$WORK/include-locked/resources/pipelines/include.json"
echo '"included"' > "$WORK/include-locked/resources/locked/part.json.inc"
cp "$(find "$PREFIX" -path '*/pipelines/MetOp.json' -print -quit)" "$WORK/include-locked/resources/pipelines/"
chmod 000 "$WORK/pipelines-locked/resources/pipelines" "$WORK/include-locked/resources/locked"
chmod 444 "$WORK/pipelines-listed/resources/pipelines"
run_in "$([ $EXERCISED = 1 ] && echo 1 || echo 0)" pipelines-locked "$WORK/pipelines-locked" true legacy version
run_in 0 pipelines-listed "$WORK/pipelines-listed" true legacy version
run_in 0 include-locked "$WORK/include-locked" true -v legacy version
chmod 755 "$WORK/pipelines-locked/resources/pipelines" "$WORK/pipelines-listed/resources/pipelines" \
    "$WORK/include-locked/resources/locked"
check_denied pipelines-locked-reported logged pipelines-locked "Couldn't load pipelines! Was trying : resources/pipelines (Permission denied)"
check_denied pipelines-listed-reported logged pipelines-listed "Entries in resources/pipelines that cannot be checked are skipped: 1, the first resources/pipelines/listed.json (Permission denied)"
check_denied include-locked-reported logged include-locked "Could not include resources/pipelines/../locked/part.json.inc! (Permission denied)"
check include-locked-other-pipelines logged include-locked " - metop_ahrpt$"

# 6. A readable current folder: its satdump_cfg.json and settings.json are used, as before
mkdir "$WORK/portable"
cp "$(find "$PREFIX" -name satdump_cfg.json -print -quit)" "$WORK/portable/"
cp "$HOME/.config/satdump/settings.json" "$WORK/portable/"
run_in 0 portable "$WORK/portable" true -v legacy version
check portable-config logged portable "Loading config satdump_cfg.json"
check portable-settings logged portable "Loading user settings settings.json"
check portable-no-debug-line not_logged portable "in the current folder"

# Nothing above ended on an unhandled error
for log in "$WORK"/*.log; do
    name=$(basename "$log" .log)
    check "$name-no-unhandled-error" not_logged "$name" "stopped on an error that was not handled"
done

if [ $EXERCISED = 0 ] && [ "${SATDUMP_REQUIRE_UNREADABLE:-0}" = 1 ]; then
    echo "FAIL the unreadable-folder runs were not exercised (see above), and SATDUMP_REQUIRE_UNREADABLE=1 asks for them"
    exit 1
fi
[ $FAILED = 0 ] && [ $EXERCISED = 0 ] && exit 77
exit $FAILED
