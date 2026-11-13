#!/bin/bash
# Fork: ABI-1 plugins must be refused before loader() or init() can call stale virtual slots.
PLUGIN=${2:?usage: $0 <install prefix> <old ABI test plugin>}
. "$(dirname "$0")/common.sh"
mkdir -p plugins
cp "$PLUGIN" plugins/ || exit 1
run 0 old-abi-rejected legacy version
check mismatch-reported grep -q "Plugin ABI mismatch!" "$WORK/old-abi-rejected.log"
check loader-not-called bash -c '! grep -q "Old ABI plugin loader was called" "$1"' _ "$WORK/old-abi-rejected.log"
check init-not-called bash -c '! grep -q "Old ABI plugin init was called" "$1"' _ "$WORK/old-abi-rejected.log"
exit "$FAILED"
