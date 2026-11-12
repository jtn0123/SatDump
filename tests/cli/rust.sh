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

exit $FAILED
