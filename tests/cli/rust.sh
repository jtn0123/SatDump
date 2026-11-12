#!/bin/bash
# Rust build (-DSATDUMP_WITH_RUST=ON): the Rust code stays hidden inside libsatdump_core.so,
# the install needs no extra libraries, and SATDUMP_NO_RUST=1 switches back to the C++ code.
. "$(dirname "$0")/common.sh"

CORE=$(find "$PREFIX" -name 'libsatdump_core.so' -print -quit)
[ -n "$CORE" ] || { echo "FAIL no libsatdump_core.so under $PREFIX"; exit 1; }
nm -D --defined-only "$CORE" > "$WORK/exported.txt"
# Rust's own names: the exported C functions, the runtime, and mangled std/core/alloc names
check no-rust-symbols-exported test "$(grep -cE ' (satdump_rs_|rust_|__rust|__rdl_|_ZN[0-9]+(std|core|alloc|satdump_rs)|_R[A-Z])' "$WORK/exported.txt")" = 0
nm -D --defined-only -C "$CORE" > "$WORK/exported-demangled.txt"
check bridge-exported grep -q 'satdump::rust::enabled()' "$WORK/exported-demangled.txt"
check no-rust-libraries-needed test "$(readelf -d "$CORE" | grep NEEDED | grep -ciE 'rust|libstd-')" = 0

head -c 102400 /dev/zero > "$WORK/in.cadu"
run 0 rust-on pipeline metop_ahrpt cadu "$WORK/in.cadu" "$WORK/on"
check rust-on-logged grep -q "Rust parts: on" "$WORK/rust-on.log"
export SATDUMP_NO_RUST=1
run 0 rust-off pipeline metop_ahrpt cadu "$WORK/in.cadu" "$WORK/off"
unset SATDUMP_NO_RUST
check rust-off-logged grep -q "Rust parts: off" "$WORK/rust-off.log"

# LRPT (M6.3): the installed Meteor plugin's Segment was built with its Rust path, so it calls
# the bridge's segment decoder (the golden pictures are the same either way, so they cannot show it)
METEOR=$(find "$PREFIX" -name 'libmeteor_support.so' -print -quit)
check meteor-plugin-found test -n "$METEOR"
if [ -n "$METEOR" ]; then
    nm -D -C "$METEOR" > "$WORK/meteor-symbols.txt"
    check meteor-segment-has-rust-path grep -q ' T meteor::msumr::lrpt::Segment::decodeRust(' "$WORK/meteor-symbols.txt"
    check meteor-segment-calls-rust grep -q ' U satdump::rust::lrpt_segment(' "$WORK/meteor-symbols.txt"
fi

exit $FAILED
