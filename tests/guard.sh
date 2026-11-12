# Guard for builds and tests on a receiving station (the Pi). Sourced by gate.sh with the
# build folder in $B. Before anything starts it refuses to run:
#   - while SatDump is working on a pass: any running `satdump` command-line process
#     (recording, live decoding or turning a recording into pictures), or while the pass
#     lock file exists (whatever schedules passes creates it ahead of a pass and removes it
#     afterwards; path: $SATDUMP_PASS_LOCK, default $XDG_RUNTIME_DIR/satdump-pass.lock)
#   - when less than $SATDUMP_MIN_FREE_GB (default 4) GB is free where the build folder
#     lives, so a pass always has room for its recording
# While it runs, the build and tests use the lowest CPU and disk priority, ccache is capped
# at 2 GB unless CCACHE_MAXSIZE says otherwise, and the gate's process id is in
# $XDG_RUNTIME_DIR/satdump-gate.pid so a pass scheduler can stop it (kill -TERM).
# SATDUMP_GUARD=off skips the two checks, for machines that never receive passes.

guard_fail() {
    echo "gate: not starting: $*"
    exit 1
}

RUNDIR=${XDG_RUNTIME_DIR:-/tmp}
if [ "${SATDUMP_GUARD:-on}" != off ]; then
    LOCK=${SATDUMP_PASS_LOCK:-$RUNDIR/satdump-pass.lock}
    [ -e "$LOCK" ] && guard_fail "a pass is scheduled or running ($LOCK exists)"
    # -x matches the program name exactly, not other command lines that mention it
    if pids=$(pgrep -x satdump); then
        guard_fail "SatDump is running (process $(echo $pids | tr ' ' ','))"
    fi
    MIN_GB=${SATDUMP_MIN_FREE_GB:-4}
    free_kb=$(df -Pk "$B" | awk 'NR == 2 { print $4 }')
    [ -n "$free_kb" ] && [ "$free_kb" -lt $((MIN_GB * 1024 * 1024)) ] &&
        guard_fail "only $((free_kb / 1024 / 1024)) GB free under $B (needs $MIN_GB GB; set SATDUMP_MIN_FREE_GB to change)"
fi

renice -n 19 -p $$ > /dev/null 2>&1
command -v ionice > /dev/null && ionice -c 3 -p $$ > /dev/null 2>&1
export CCACHE_MAXSIZE=${CCACHE_MAXSIZE:-2G}
echo $$ > "$RUNDIR/satdump-gate.pid" 2> /dev/null &&
    trap 'rm -f "$RUNDIR/satdump-gate.pid"' EXIT
