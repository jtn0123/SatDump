#!/bin/bash
# Scan the bytes of every payload/control file, including ELF debug sections, as well
# as entry names and symlink targets. Print only offending file names, never their data.
# Usage: bash check_paths.sh <unpacked root> <source dir> <build dir>
set -uo pipefail
export LC_ALL=C
root=${1:?unpacked root}
source_dir=${2:?source dir}
build_dir=${3:?build dir}
[ -d "$root" ] || { echo "no unpacked root"; exit 1; }
patterns=$(mktemp)
trap 'rm -f "$patterns" "$patterns.files"' EXIT
find "$root" -mindepth 1 -print0 > "$patterns.files" || exit 1
printf '%s\n' '/home/' "$source_dir" "$build_dir" > "$patterns"
failed=0
while IFS= read -r -d '' f; do
    rel=${f#"$root"/}
    target=""
    [ ! -L "$f" ] || target=$(readlink "$f")
    if printf '/%s\n%s\n' "$rel" "$target" | grep -Fq -f "$patterns"; then
        echo "build/home path in entry: $rel"
        failed=1
    fi
    [ -f "$f" ] && [ ! -L "$f" ] || continue
    grep -aFq -f "$patterns" "$f"
    rc=$?
    if [ "$rc" = 0 ]; then
        echo "build/home path in file: $rel"
        failed=1
    elif [ "$rc" != 1 ]; then
        echo "cannot scan file: $rel"
        failed=1
    fi
done < "$patterns.files"
exit "$failed"
