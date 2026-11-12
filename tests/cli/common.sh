# Shared setup for the cli tests: an installed SatDump, run from an empty folder with its own
# HOME and no network downloads. Source it with the install prefix as $1.
set -u
PREFIX=${1:?usage: $0 <install prefix>}
SD="$PREFIX/bin/satdump"
[ -x "$SD" ] || { echo "no satdump at $SD (run tests/gate.sh, which installs first)"; exit 1; }
export LD_LIBRARY_PATH="$PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
export HOME="$WORK/home"
mkdir -p "$HOME/.config/satdump"
# Never download orbit or Earth-rotation data during tests
cat > "$HOME/.config/satdump/settings.json" <<'JSON'
{ "satdump_general": { "kepler_update_interval": { "value": "Never" }, "iers_auto_update": { "value": false } } }
JSON
cd "$WORK"
FAILED=0

# run <expected exit code> <name> <args...>: run satdump and compare its exit code
run() {
    local want=$1 name=$2
    shift 2
    "$SD" "$@" > "$WORK/$name.log" 2>&1
    local got=$?
    if [ "$got" = "$want" ]; then
        echo "ok   $name (exit $got)"
    else
        echo "FAIL $name: exit $got, expected $want"
        sed 's/\x1b\[[0-9;]*m//g' "$WORK/$name.log" | tail -8 | sed 's/^/     /'
        FAILED=1
    fi
}
