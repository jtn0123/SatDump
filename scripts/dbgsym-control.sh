#!/bin/bash
# CPack 3.31's dbgsym md5sums uses absolute staging names. Debian expects paths relative
# to the package root. Fix that control member without changing/recompressing the payload.
# Already-relative checksums (including those from a fixed CPack) are left untouched.
set -euo pipefail
export LC_ALL=C
package=$(realpath "${1:?debug-symbols package}")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir "$work/control"
dpkg-deb -e "$package" "$work/control"
sums="$work/control/md5sums"
sed -E 's@^([0-9a-f]{32}  ).*/(usr/lib/debug/\.build-id/[0-9a-f]{2}/[0-9a-f]+\.debug)$@\1\2@' "$sums" > "$work/relative"
if [ ! -s "$work/relative" ] || grep -qvE '^[0-9a-f]{32}  usr/lib/debug/\.build-id/[0-9a-f]{2}/[0-9a-f]+\.debug$' "$work/relative"; then
    echo "dbgsym-control: unexpected debug checksum entries" >&2
    exit 1
fi
cmp -s "$sums" "$work/relative" && exit 0
# CPack's control archive is gzip, independently of its payload compression setting.
ar t "$package" | grep -qx control.tar.gz || { echo "dbgsym-control: expected control.tar.gz" >&2; exit 1; }
cat "$work/relative" > "$sums"
tar -C "$work/control" --owner=0 --group=0 --numeric-owner -czf "$work/control.tar.gz" .
ar r "$package" "$work/control.tar.gz"
echo "dbgsym-control: normalized checksum paths"
