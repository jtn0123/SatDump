"""Warning ratchet: the number of compiler warnings may only go down.

Usage: ratchet.py <build log> <source root> <baseline file> [--update]

A warning is keyed by its file (relative to the source root) and its -W flag, without line
numbers, so moving code does not change the key. The same warning seen from several
compile units counts once. The check fails when a key has more warnings than the baseline
(a new warning) or fewer (a fixed warning: run gate.sh with --update to lower the baseline).
"""
import os
import re
import sys
from collections import Counter

GCC = re.compile(r"^(?P<path>[^\s:][^:]*):(?P<line>\d+):(?P<col>\d+): warning: (?P<msg>.*?)(?: \[(?P<flag>-W[^\]]+)\])?$")
ESC = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")


def keys_from_log(log_path, root):
    seen = set()
    counts = Counter()
    with open(log_path, errors="replace") as fh:
        for raw in fh:
            line = ESC.sub("", raw.rstrip("\n"))
            if "warning:" not in line:
                continue
            m = GCC.match(line)
            if m:
                path = os.path.normpath(m["path"])
                if os.path.isabs(path) and path.startswith(root + os.sep):
                    path = os.path.relpath(path, root)
                what = m["flag"] or re.sub(r"\d+", "N", m["msg"])[:80]
                ident = (path, m["line"], m["col"], what)
                key = f"{path} {what}"
            else:  # linker and tool warnings have no file:line
                what = re.sub(r"\d+", "N", line.split("warning:", 1)[1].strip())[:100]
                ident = ("", "", "", what)
                key = f"(other) {what}"
            if ident not in seen:
                seen.add(ident)
                counts[key] += 1
    return counts


def read_baseline(path):
    counts = Counter()
    with open(path) as fh:
        for line in fh:
            line = line.rstrip("\n")
            if line and not line.startswith("#"):
                n, key = line.split(" ", 1)
                counts[key] = int(n)
    return counts


def write_baseline(path, counts):
    with open(path, "w") as fh:
        fh.write("# Warning baseline for tests/gate.sh (count, file, flag). Only lower it.\n")
        for key in sorted(counts):
            fh.write(f"{counts[key]} {key}\n")


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 1
    log_path, root, base_path = sys.argv[1], os.path.realpath(sys.argv[2]), sys.argv[3]
    update = "--update" in sys.argv[4:]
    now = keys_from_log(log_path, root)
    total = sum(now.values())
    if not os.path.exists(base_path):
        if update:
            write_baseline(base_path, now)
            print(f"ratchet: new baseline with {total} warnings in {len(now)} keys")
            return 0
        print(f"ratchet: no baseline at {base_path}; run gate.sh with --update to create it")
        return 1
    base = read_baseline(base_path)
    worse = {k: (base.get(k, 0), n) for k, n in now.items() if n > base.get(k, 0)}
    better = {k: (n, now.get(k, 0)) for k, n in base.items() if now.get(k, 0) < n}
    for k, (was, is_) in sorted(worse.items()):
        print(f"ratchet: NEW  {k}: {was} -> {is_}")
    if worse:
        print(f"ratchet: FAIL, {sum(b - a for a, b in worse.values())} new warning(s)")
        return 1
    if better:
        for k, (was, is_) in sorted(better.items()):
            print(f"ratchet: fixed {k}: {was} -> {is_}")
        if update:
            write_baseline(base_path, now)
            print(f"ratchet: baseline lowered to {total}")
            return 0
        print("ratchet: FAIL, warnings were fixed; run gate.sh with --update to lower the baseline")
        return 1
    print(f"ratchet: ok, {total} warnings (baseline {sum(base.values())})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
