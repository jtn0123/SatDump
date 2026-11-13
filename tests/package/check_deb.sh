#!/bin/bash
# Fork: checks a built satdump-fork package (scripts/build-deb.sh) without installing it.
# It is not part of tests/gate.sh, as the package is a separate full build. Run it as a normal
# user on arm64 Debian 13 (the Pi, or the machine that built the package):
#
#   tests/package/check_deb.sh <satdump-fork_*.deb> <baseline prefix> <recording>
#
#   <baseline prefix>  a SatDump install to compare the products with, for example one made
#                      with `cmake --install <build folder> --prefix <dir>` from the commit before
#   <recording>        a MetOp AHRPT baseband recording, cs16 at 10 Msps (PIPELINE, SAMPLERATE
#                      and FORMAT in the environment choose another kind), for example 10 s of a pass
#
# The debug-symbols package scripts/build-deb.sh makes with it is taken from the same folder
# (satdump-fork-dbgsym_<version>_<arch>.ddeb; DBGSYM=<file> names another).
# Every payload/control file in both packages is scanned for source/build and home paths.
# For packages copied away from their build folder, set PACKAGE_SOURCE_DIR and
# PACKAGE_BUILD_DIR to the original roots (normally read from adjacent CMakeCache.txt).
#
# It checks, in order:
#   1. dpkg-deb --info: the name is satdump-fork, the version is valid, the architecture is
#      arm64 (ARCH=... for another), Depends was generated (it has libc6) and names no -dev
#      package and none of SatDump's own libraries, and it provides, conflicts with and
#      replaces satdump. The .deb is at most MAX_MIB MiB (default 110; stripped it is about
#      93 MiB, unstripped it was 330 MiB)
#   2. dpkg-deb --contents: satdump, libsatdump_core.so, the plugins (MetOp/NOAA and Meteor
#      among them), every file of resources/ in this source tree (the themes too) and
#      satdump_cfg.json (default theme Soft OLED); no build-folder paths
#   3. unpacked into a temporary folder, every program, library and plugin finds all the
#      libraries it needs: ldd with the unpacked library folders on LD_LIBRARY_PATH, and again
#      without it (the programs find them next to themselves); SatDump's own libraries always
#      come from the package; and every system library they link against directly is in a
#      package that Depends names
#   4. every program, library and plugin is stripped (no .debug_* sections, no .symtab) and has
#      a GNU build-id. The debug-symbols package is satdump-fork-dbgsym,
#      of the same version and architecture, needs exactly this version of satdump-fork, lists
#      those build-ids (Build-Ids) and holds one /usr/lib/debug/.build-id/xx/yyyy.debug
#      file for each (xx the first two hex digits of the build-id, yyyy the rest) and nothing
#      else; each of those files has that build-id, a symbol table and debug information
#      (.debug_info, .debug_line). If gdb is installed, it must find satdump's line numbers
#      through that folder
#   5. apt-get install --simulate (no root needed): the package's dependencies resolve on this
#      system, alone, together with packages.runner (as the Dockerfile installs it), and with
#      its debug-symbols package
#   6. the unpacked satdump, run with no LD_LIBRARY_PATH from another folder, loads its plugins
#      from the package and decodes the recording into the same products as the baseline
#      install: the same files, byte for byte (the .cadu files, the raw frames, are left out)
#
# Exit code 0 when every check passed, 1 otherwise. Needs dpkg-deb, apt-get, ldd and sha256sum
# (all in a normal Debian install), readelf (binutils) and room in TMPDIR for the unpacked
# packages and the two runs' products (about 2 GB for a 10-second recording; the unpacked
# debug symbols are removed after part 4). gdb is optional. KEEP=1 keeps the temporary folder
# (logs, products, debug symbols).
set -uo pipefail
export LC_ALL=C
usage="usage: $0 <satdump-fork_*.deb> <baseline prefix> <recording>"
DEB=$(realpath "${1:?$usage}") || exit 1
BASE=$(realpath "${2:?$usage}") || exit 1
REC=$(realpath "${3:?$usage}") || exit 1
[ -f "$DEB" ] || { echo "no package at $DEB"; exit 1; }
[ -x "$BASE/bin/satdump" ] || { echo "no satdump at $BASE/bin/satdump"; exit 1; }
[ -f "$REC" ] || { echo "no recording at $REC"; exit 1; }
SRC=$(cd "$(dirname "$0")/../.." && pwd)
PIPELINE=${PIPELINE:-metop_ahrpt}
SAMPLERATE=${SAMPLERATE:-10e6}
FORMAT=${FORMAT:-cs16}
ARCH=${ARCH:-arm64}
MAX_MIB=${MAX_MIB:-110}
# Adjacent CMakeCache.txt records the actual build roots. For copied packages,
# PACKAGE_SOURCE_DIR/PACKAGE_BUILD_DIR can supply them; defaults are this tree/folder.
cache="$(dirname "$DEB")/CMakeCache.txt"
package_source=$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$cache" 2>/dev/null)
package_build=$(sed -n 's/^CMAKE_CACHEFILE_DIR:INTERNAL=//p' "$cache" 2>/dev/null)
PACKAGE_SOURCE_DIR=${PACKAGE_SOURCE_DIR:-${package_source:-$SRC}}
PACKAGE_BUILD_DIR=${PACKAGE_BUILD_DIR:-${package_build:-$(dirname "$DEB")}}

WORK=$(mktemp -d)
trap '[ -n "${KEEP:-}" ] && echo "kept $WORK" || rm -rf "$WORK"' EXIT
ROOT="$WORK/root"
FAILED=0
ok() { echo "ok   $*"; }
fail() {
    echo "FAIL $*"
    FAILED=1
}
section() { printf '\n== %s\n' "$*"; }
indent() { sed 's/^/     /'; }
check_paths() {
    local label=$1 dir=$2
    if bash "$SRC/tests/package/check_paths.sh" "$dir" "$PACKAGE_SOURCE_DIR" "$PACKAGE_BUILD_DIR" > "$WORK/paths.log"; then
        ok "$label: no source/build or home paths in any file, entry or symlink"
    else
        fail "$label: source/build or home paths found"
        cat "$WORK/paths.log" | indent
    fi
}
check_sums() {
    local label=$1 dir=$2 control=$3
    if (cd "$dir" && md5sum --check --status "$control/md5sums"); then
        ok "$label: control checksums match every payload file"
    else
        fail "$label: invalid control checksums"
    fi
}

section "1. Package information"
dpkg-deb --info "$DEB" | indent
field() { dpkg-deb --field "$DEB" "$1"; }
# names <field>: the package names in a relationship field, one per line
names() { field "$1" | tr ',|' '\n\n' | sed 's/([^)]*)//; s/^[[:space:]]*//; s/[[:space:]]*$//' | grep -v '^$'; }

[ "$(field Package)" = satdump-fork ] && ok "name satdump-fork" || fail "name is '$(field Package)'"
version=$(field Version)
if dpkg --validate-version "$version" 2> /dev/null && [[ $version =~ ^[^-]+-[^-]+$ ]]; then
    ok "version $version (Debian revision ${version##*-})"
else
    fail "version '$version' is not a valid Debian version with one revision"
fi
[ "$(field Architecture)" = "$ARCH" ] && ok "architecture $ARCH" || fail "architecture is '$(field Architecture)', expected $ARCH"
if names Depends | grep -qx libc6; then
    ok "Depends generated by dpkg-shlibdeps ($(names Depends | wc -l) entries, libc6 among them)"
else
    fail "Depends was not generated (no libc6): '$(field Depends)'"
fi
bad=$(names Depends | grep -E -- '-dev$|^libsatdump|_support$' || true)
[ -z "$bad" ] && ok "Depends names no -dev package and none of SatDump's own libraries" ||
    fail "Depends names: $(echo $bad)"
for f in Provides Conflicts Replaces; do
    names "$f" | grep -qx satdump && ok "$f: satdump" || fail "$f does not name satdump: '$(field "$f")'"
done
# Stripped, the package is a fraction of its unstripped size (330 MiB)
size=$(stat -c %s "$DEB")
mib=$(((size + 1048575) / 1048576))
[ "$size" -le $((MAX_MIB * 1048576)) ] && ok "size $mib MiB ($size bytes), at most $MAX_MIB MiB" ||
    fail "size $mib MiB ($size bytes), more than $MAX_MIB MiB: are its files stripped?"
# The debug-symbols package, next to the .deb (as CPack names it) unless DBGSYM names it
DBGSYM=${DBGSYM:-$(dirname "$DEB")/$(field Package)-dbgsym_${version#*:}_$(field Architecture).ddeb}

section "2. Package contents"
# The path of each entry (dpkg-deb lists: mode owner size date time path [-> target])
dpkg-deb --contents "$DEB" | sed -E 's/^([^ ]+ +){5}//; s/ -> .*$//; s|^\./|/|' > "$WORK/contents.txt"
echo "     $(wc -l < "$WORK/contents.txt") entries"
# has <name> <regular expression>: at least one entry matches
has() {
    local n more=""
    n=$(grep -cE "$2" "$WORK/contents.txt")
    [ "$n" -gt 1 ] && more=" and $((n - 1)) more"
    [ "$n" -gt 0 ] && ok "$1: $(grep -E "$2" "$WORK/contents.txt" | head -1)$more" || fail "no $1 in the package"
}
has "satdump" '^/usr/bin/satdump$'
has "libsatdump_core.so" '^/usr/lib/(.+/)?libsatdump_core\.so$'
has "plugins" '^/usr/lib/(.+/)?satdump/plugins/[^/]+\.so$'
has "MetOp/NOAA plugin" '/satdump/plugins/libnoaa_metop_support\.so$'
has "Meteor plugin" '/satdump/plugins/libmeteor_support\.so$'
has "satdump_cfg.json" '^/usr/share/satdump/satdump_cfg\.json$'
has "Soft OLED theme" '^/usr/share/satdump/resources/themes/Soft OLED\.json$'
missing=0
total=0
while IFS= read -r -d '' f; do
    total=$((total + 1))
    grep -qxF "/usr/share/satdump/resources/${f#./}" "$WORK/contents.txt" ||
        { [ $missing -lt 5 ] && echo "     missing: resources/${f#./}"; missing=$((missing + 1)); }
done < <(cd "$SRC/resources" && find . -type f -print0)
[ $missing = 0 ] && ok "all $total files of resources/ (themes, fonts, pipelines)" ||
    fail "$missing of the $total files of resources/ are missing"
build_paths=$(grep -E '(^|/)(build[-_][^/]*|_CPack_Packages|CMakeFiles|check-prefix)(/|$)|^/home/|^/tmp/|\.o$' "$WORK/contents.txt" || true)
[ -z "$build_paths" ] && ok "no build-folder paths" || fail "build-folder paths in the package: $(echo "$build_paths" | head -3)"

section "3. Libraries of the unpacked package"
dpkg-deb -x "$DEB" "$ROOT" || { fail "dpkg-deb -x failed"; exit 1; }
dpkg-deb -e "$DEB" "$WORK/main-control" || { fail "cannot unpack main control files"; exit 1; }
check_paths "main package" "$ROOT"
check_paths "main control" "$WORK/main-control"
check_sums "main package" "$ROOT" "$WORK/main-control"
grep -q '"value": "Soft OLED"' "$ROOT/usr/share/satdump/satdump_cfg.json" &&
    ok "satdump_cfg.json: default theme Soft OLED" || fail "satdump_cfg.json does not default to Soft OLED"
libpath=$(find "$ROOT" \( -type f -o -type l \) -name '*.so*' -printf '%h\n' | sort -u | paste -sd:)
# check_ldd <label> <LD_LIBRARY_PATH or empty>
check_ldd() {
    local label=$1 n=0 bad=0 f out
    while IFS= read -r -d '' f; do
        head -c 4 "$f" | grep -q $'\x7fELF' || continue
        n=$((n + 1))
        if [ -n "$2" ]; then
            out=$(LD_LIBRARY_PATH="$2" ldd "$f" 2>&1)
        else
            out=$(env -u LD_LIBRARY_PATH ldd "$f" 2>&1)
        fi
        # Not found, or one of SatDump's own libraries taken from outside the package
        local wrong
        wrong=$(echo "$out" | grep -E 'not found|(libsatdump_[a-z]+|lib[a-z0-9_]+_support)\.so[.0-9]* => ' | grep -v "=> $ROOT/")
        if [ -n "$wrong" ]; then
            bad=$((bad + 1))
            echo "     ${f#"$ROOT"}:"
            echo "$wrong" | sed 's/^[[:space:]]*/       /'
        fi
    done < <(find "$ROOT" -type f -print0)
    [ $n -gt 0 ] && [ $bad = 0 ] && ok "$label: all $n programs and libraries find every library (none 'not found')" ||
        fail "$label: $bad of $n programs and libraries miss a library or take SatDump's from elsewhere"
}
check_ldd "with LD_LIBRARY_PATH=<the unpacked library folders>" "$libpath"
check_ldd "with no LD_LIBRARY_PATH" ""

# Every system library a program, library or plugin of the package links against directly
# (its NEEDED entries) is in a package that Depends names, so installing the package brings it
# owner <file>: the Debian package a file belongs to (merged /usr: try both spellings)
owner() {
    local p
    p=$(realpath "$1")
    for q in "$p" "${p#/usr}" "/usr$p"; do
        dpkg -S "$q" 2> /dev/null | grep -v '^diversion' | head -1 | cut -d: -f1 | grep . && return
    done
}
if command -v readelf > /dev/null; then
    declare -A syslibs
    while IFS= read -r -d '' f; do
        head -c 4 "$f" | grep -q $'\x7fELF' || continue
        map=$(env -u LD_LIBRARY_PATH ldd "$f" 2>&1)
        for so in $(readelf -d "$f" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'); do
            path=$(awk -v s="$so" '$1 == s && $2 == "=>" { print $3 }' <<< "$map")
            [ -n "$path" ] || path=$(awk -v s="$so" '$1 ~ "/" s "$" { print $1 }' <<< "$map")
            case $path in
                "$ROOT"/*) ;;
                "") syslibs["$so"]="" ;;
                *) syslibs["$so"]=$path ;;
            esac
        done
    done < <(find "$ROOT" -type f -print0)
    depends=$(names Depends)
    notlisted=""
    for so in "${!syslibs[@]}"; do
        pkg=""
        [ -n "${syslibs[$so]}" ] && pkg=$(owner "${syslibs[$so]}")
        grep -qxF "${pkg:-?}" <<< "$depends" || notlisted+=" $so(${pkg:-no package})"
    done
    [ -z "$notlisted" ] && ok "all ${#syslibs[@]} system libraries linked directly come from packages Depends names" ||
        fail "linked directly, but their packages are not in Depends:$notlisted"
else
    fail "readelf (package binutils) is needed to compare the linked libraries with Depends"
fi

section "4. Stripped ELF files and matching debug symbols"
# Do not rely on file(1)'s stripped label: inspect sections and the actual build-id note.
build_id() { readelf -n "$1" 2>/dev/null | sed -n 's/.*Build ID: //p'; }
elfs=0
: > "$WORK/build-ids"
while IFS= read -r -d '' f; do
    head -c 4 "$f" | grep -q $'\x7fELF' || continue
    elfs=$((elfs + 1))
    sections=$(readelf -SW "$f") || { fail "cannot read ELF sections: ${f#"$ROOT"}"; continue; }
    if grep -qE '[[:space:]]\.(z?debug_[^[:space:]]*|symtab)[[:space:]]' <<< "$sections"; then
        fail "unstripped ELF (debug sections or .symtab): ${f#"$ROOT"}"
    fi
    id=$(build_id "$f")
    if [[ $id =~ ^[0-9a-f]{40}$ ]]; then
        echo "$id" >> "$WORK/build-ids"
    else
        fail "missing GNU SHA-1 build-id: ${f#"$ROOT"}"
    fi
done < <(find "$ROOT" -type f -print0)
[ "$elfs" -gt 0 ] && ok "inspected stripping and GNU build-ids of $elfs ELF files" || fail "no ELF files"
sort -u "$WORK/build-ids" -o "$WORK/build-ids"

if [ ! -f "$DBGSYM" ]; then
    fail "no debug-symbols package: $DBGSYM"
else
    dbgfield() { dpkg-deb --field "$DBGSYM" "$1"; }
    [ "$(dbgfield Package)" = satdump-fork-dbgsym ] && ok "name satdump-fork-dbgsym" || fail "wrong debug package name"
    [ "$(dbgfield Version)" = "$version" ] && [ "$(dbgfield Architecture)" = "$(field Architecture)" ] &&
        ok "debug package version and architecture match" || fail "debug package version or architecture differs"
    [ "$(dbgfield Depends)" = "satdump-fork (= $version)" ] && ok "debug package needs this exact main version" ||
        fail "debug package Depends: $(dbgfield Depends)"
    dbgfield Build-Ids | tr ' ' '\n' | sed '/^$/d' | sort -u > "$WORK/debug-build-ids"
    cmp -s "$WORK/build-ids" "$WORK/debug-build-ids" && ok "Build-Ids matches every main ELF" ||
        fail "debug package Build-Ids differs from main ELF build-ids"
    DEBUG="$WORK/debug"
    if dpkg-deb -x "$DBGSYM" "$DEBUG"; then
        check_paths "debug package" "$DEBUG"
        if dpkg-deb -e "$DBGSYM" "$WORK/debug-control"; then
            check_paths "debug control" "$WORK/debug-control"
            check_sums "debug package" "$DEBUG" "$WORK/debug-control"
        else
            fail "cannot unpack debug control files"
        fi
        : > "$WORK/expected-debug-files"
        matched=0
        while IFS= read -r id; do
            rel="usr/lib/debug/.build-id/${id:0:2}/${id:2}.debug"
            echo "$rel" >> "$WORK/expected-debug-files"
            f="$DEBUG/$rel"
            if [ ! -f "$f" ]; then
                fail "no debug file for build-id $id"
                continue
            fi
            [ "$(build_id "$f")" = "$id" ] || fail "debug ELF build-id differs: $rel"
            sections=$(readelf -SW "$f")
            for section_name in .debug_info .debug_line .symtab; do
                grep -qF " $section_name " <<< "$sections" || fail "$rel lacks $section_name"
            done
            matched=$((matched + 1))
        done < "$WORK/build-ids"
        (cd "$DEBUG" && find . -type f -printf '%P\n' | sort) > "$WORK/debug-files"
        sort -o "$WORK/expected-debug-files" "$WORK/expected-debug-files"
        cmp -s "$WORK/expected-debug-files" "$WORK/debug-files" && ok "$matched debug files at matching build-id paths" ||
            fail "debug package files differ from the expected build-id paths"
        if command -v gdb > /dev/null; then
            # Disable init files and downloads; symbols must come from the unpacked package.
            if DEBUGINFOD_URLS= gdb -nx -batch -iex 'set auto-load off' -ex 'set debuginfod enabled off' \
                -ex "set debug-file-directory $DEBUG/usr/lib/debug" -ex "file $ROOT/usr/bin/satdump" \
                -ex 'info line main' > "$WORK/gdb.log" 2>&1 && grep -qE '^Line [0-9]+ of ' "$WORK/gdb.log"; then
                ok "gdb resolves main to a source line through the packaged build-id symbols"
            else
                fail "gdb could not resolve main using the debug package"
                cat "$WORK/gdb.log" | indent
            fi
        fi
        [ -n "${KEEP:-}" ] || rm -rf "$DEBUG"
    else
        fail "cannot unpack debug-symbols package"
    fi
fi

section "5. apt-get install --simulate"
# simulate <label> <packages...>
simulate() {
    local label=$1
    shift
    if ! apt-get install --simulate "$@" > "$WORK/apt.log" 2>&1; then
        fail "$label: apt-get cannot install it (its dependencies do not resolve)"
        grep -vE '^(Reading|Building|Note|NOTE|Inst|Conf) ' "$WORK/apt.log" | tail -15 | indent
    elif ! grep -q '^Inst satdump-fork ' "$WORK/apt.log"; then
        fail "$label: apt-get would not install a package called satdump-fork"
    else
        ok "$label: dependencies resolve ($(grep -c '^Inst ' "$WORK/apt.log") to install, $(grep -c '^Remv ' "$WORK/apt.log") to remove)"
        grep -E '^(Inst|Remv) ' "$WORK/apt.log" | indent
    fi
}
simulate "the package" "$DEB"
[ ! -f "$DBGSYM" ] || simulate "the package with debug symbols" "$DEB" "$DBGSYM"
[ -f "$SRC/packages.runner" ] && simulate "the package with packages.runner" $(cat "$SRC/packages.runner") "$DEB"

section "6. Products of the unpacked package and of $BASE"
# new_home <label>: an empty HOME for one run, in which SatDump never downloads orbit or
# Earth-rotation data (as in tests/cli/common.sh), and an empty folder to run in
new_home() {
    mkdir -p "$WORK/home-$1/.config/satdump" "$WORK/run-$1"
    echo '{ "satdump_general": { "kepler_update_interval": { "value": "Never" }, "iers_auto_update": { "value": false } } }' \
        > "$WORK/home-$1/.config/satdump/settings.json"
}
# decode <label> <satdump> <LD_LIBRARY_PATH or empty>: products in $WORK/<label>
decode() {
    local label=$1
    new_home "$label"
    local env=(env -u LD_LIBRARY_PATH HOME="$WORK/home-$label")
    [ -n "$3" ] && env+=(LD_LIBRARY_PATH="$3")
    (cd "$WORK/run-$label" && "${env[@]}" "$2" pipeline "$PIPELINE" baseband "$REC" "$WORK/$label" \
        --samplerate="$SAMPLERATE" --baseband_format="$FORMAT") 2>&1 | sed 's/\x1b\[[0-9;]*m//g' > "$WORK/$label.log"
    local rc=${PIPESTATUS[0]}
    [ "$rc" = 0 ] && ok "$label: satdump exit 0" || { fail "$label: satdump exit $rc"; tail -8 "$WORK/$label.log" | indent; }
}
# sums <label>: the sha256 of every product but the .cadu files
sums() { (cd "$WORK/$1" && find . -type f ! -name '*.cadu' -print0 | sort -z | xargs -0 -r sha256sum); }

# Where the unpacked satdump takes SatDump's libraries and plugins from, as the dynamic loader
# records it (LD_DEBUG=libs) in a short run with no LD_LIBRARY_PATH, from another folder.
# Plugins are found through the same path lookup as the resources (next to the program).
new_home where
: > "$WORK/empty.cadu"
(cd "$WORK/run-where" && env -u LD_LIBRARY_PATH HOME="$WORK/home-where" LD_DEBUG=libs LD_DEBUG_OUTPUT="$WORK/lddebug" \
    "$ROOT/usr/bin/satdump" pipeline metop_ahrpt cadu "$WORK/empty.cadu" "$WORK/where-out") > "$WORK/where.log" 2>&1
loaded=$(cat "$WORK"/lddebug.* 2> /dev/null | sed -n 's/.*calling init: //p' | grep -E 'libsatdump_|/satdump/plugins/' | sort -u)
outside=$(grep -v "^$ROOT/" <<< "$loaded" || true)
plugins=$(grep -cE "^$ROOT/usr/lib/.*/satdump/plugins/[^/]+\.so$" <<< "$loaded")
if [ "$plugins" -gt 0 ] && [ -z "$outside" ] && grep -q "^$ROOT/.*/libsatdump_core\.so" <<< "$loaded"; then
    ok "the unpacked satdump loaded libsatdump_core and $plugins plugins, all from the package"
else
    fail "the unpacked satdump loaded $plugins plugins from the package; from elsewhere: '$outside'"
fi

decode baseline "$BASE/bin/satdump" "$BASE/lib"
decode package "$ROOT/usr/bin/satdump" ""
sums baseline > "$WORK/baseline.sums"
sums package > "$WORK/package.sums"
n=$(wc -l < "$WORK/baseline.sums")
if [ "$n" -gt 0 ] && cmp -s "$WORK/baseline.sums" "$WORK/package.sums"; then
    ok "all $n products identical (every file but the .cadu, sha256)"
else
    fail "products differ ($n from the baseline, $(wc -l < "$WORK/package.sums") from the package):"
    diff "$WORK/baseline.sums" "$WORK/package.sums" | head -20 | indent
fi

section "Result"
[ $FAILED = 0 ] && echo "check_deb: PASSED" || echo "check_deb: FAILED"
exit $FAILED
