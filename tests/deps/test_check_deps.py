"""Fork: self-test for check_deps.py. Copies the small fake tree in selftest_tree/ to a temporary
folder, moves its list to the default place (tests/deps/third_party.json; in the source it sits at
the top of selftest_tree/, because .gitignore ignores folders named deps), and shows that the check
  - passes on a matching list (and ignores .DS_Store and Thumbs.db),
  - fails, with a plain message, on a wrong version in the list, a changed file, a new file in
    a library, a missing path or version file, a version line that changed shape, an unlisted
    new library folder or file, a missing scanned folder, a missing DEPENDENCIES.md, a library
    DEPENDENCIES.md does not mention, a table row that does not show the listed version, and a
    broken list entry (missing key, version that is not text, name or path listed twice, path
    outside the source root),
  - uses the list given with --list,
  - never writes the list without --update,
and that --update fixes a hash and a readable version (printing each change), reminds about a
hand-recorded version, and does not hide an unlisted folder.

Usage: test_check_deps.py   (exit 0 when it passes)"""
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
CHECK = os.path.join(HERE, "check_deps.py")
TREE = os.path.join(HERE, "selftest_tree")
LIST = os.path.join("tests", "deps", "third_party.json")


def run(*args):
    proc = subprocess.run([sys.executable, "-I", "-B", CHECK, *args],
                          capture_output=True, text=True, check=False)
    return proc.returncode, proc.stdout + proc.stderr


def documented_hash(root, paths):
    """The hash as check_deps.py's docstring defines it, computed here on its own: every file
    under the paths, sorted by path relative to the root, each as path NUL size NUL bytes."""
    files = set()
    for rel in paths:
        full = os.path.join(root, rel)
        if os.path.isfile(full):
            files.add(rel)
            continue
        for dirpath, _, names in os.walk(full):
            for name in names:
                if name not in (".DS_Store", "Thumbs.db"):
                    files.add(os.path.relpath(os.path.join(dirpath, name), root).replace(os.sep, "/"))
    digest = hashlib.sha256()
    for rel in sorted(files):
        with open(os.path.join(root, rel), "rb") as fh:
            data = fh.read()
        digest.update(f"{rel}\0{len(data)}\0".encode() + data)
    return digest.hexdigest()


def read(root, rel):
    with open(os.path.join(root, rel), encoding="utf-8") as fh:
        return fh.read()


def write(root, rel, text):
    path = os.path.join(root, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(text)


def replace(root, rel, old, new):
    text = read(root, rel)
    assert old in text, f"fixture lost {old!r}"
    write(root, rel, text.replace(old, new))


def load_list(root):
    return json.loads(read(root, LIST))


def save_list(root, data):
    write(root, LIST, json.dumps(data, indent=2) + "\n")


def lib(data, name):
    return next(entry for entry in data["libraries"] if entry["name"] == name)


def edit_list(change):
    """A tree change that edits the copied list with change(data)."""
    def apply(root):
        data = load_list(root)
        change(data)
        save_list(root, data)
    return apply


def set_list_version(data):
    lib(data, "alpha")["version"] = "1.2.4"


def drop_sha256(data):
    del lib(data, "alpha")["sha256"]


def list_alpha_twice(data):
    extra = dict(lib(data, "single"))
    extra["name"] = "alpha"
    extra["paths"] = ["lib/own_helper.h"]
    data["libraries"].append(extra)


def share_a_path(data):
    extra = dict(lib(data, "single"))
    extra["name"] = "gamma"
    data["libraries"].append(extra)


def set_single_path(path):
    def change(data):
        lib(data, "single")["paths"] = [path]
    return change


def set_single_version(data):
    lib(data, "single")["version"] = "2.0"


def set_single_version_to_none(data):
    lib(data, "single")["version"] = None


def scan_a_missing_folder(data):
    data["scan"].append("gone")


def move_version_to_prose(root):
    """The row loses its version; a plain paragraph names the path and starts with the version."""
    replace(root, "DEPENDENCIES.md", "| 1.2.3 (from `alpha.h`) |", "| see below |")
    write(root, "DEPENDENCIES.md", read(root, "DEPENDENCIES.md") + "\n1.2.3 (the version of `lib/alpha`).\n")


def add_mac_and_windows_files(root):
    write(root, "lib/.DS_Store", "x")
    write(root, "lib/alpha/.DS_Store", "x")
    write(root, "lib/alpha/src/Thumbs.db", "x")


# name, change to the copied tree, exit code wanted, text the output must contain
CHECK_CASES = [
    ("matching list passes", None, 0, "check_deps: ok, 2 libraries match the list"),
    ("folder files from macOS and Windows are ignored", add_mac_and_windows_files, 0, "ok, 2 libraries"),
    ("wrong version in the list fails", edit_list(set_list_version), 1,
     "alpha: lib/alpha/alpha.h says version '1.2.3' but the list says '1.2.4'"),
    ("changed file fails", lambda r: write(r, "lib/alpha/src/a.c", "int alpha_add(int a, int b) { return a - b; }\n"),
     1, "alpha: the files changed"),
    ("new file in a library fails", lambda r: write(r, "lib/alpha/src/b.c", "int b;\n"), 1,
     "alpha: the files changed"),
    ("unlisted library folder fails", lambda r: write(r, "lib/newlib/new.h", "/* new */\n"), 1,
     "lib/newlib is not in the list: add it to tests/deps/third_party.json (a library, or a not_libraries "
     "entry for the project's own file) and DEPENDENCIES.md"),
    ("unlisted one-file library fails", lambda r: write(r, "lib/extra.hpp", "/* new */\n"), 1,
     "lib/extra.hpp is not in the list"),
    ("missing path fails", lambda r: os.remove(os.path.join(r, "lib", "single.h")), 1,
     "single: lib/single.h is missing"),
    ("missing version file fails", lambda r: os.remove(os.path.join(r, "lib", "alpha", "alpha.h")), 1,
     "alpha: the version file lib/alpha/alpha.h is missing"),
    ("missing scanned folder fails", edit_list(scan_a_missing_folder), 1,
     "the scanned folder gone is missing"),
    ("version line that changed shape fails",
     lambda r: replace(r, "lib/alpha/alpha.h", "ALPHA_VERSION_MINOR", "ALPHA_MINOR"), 1,
     "alpha: the version line was not found in lib/alpha/alpha.h"),
    ("library missing from DEPENDENCIES.md fails",
     lambda r: replace(r, "DEPENDENCIES.md", "`lib/single.h`", "`single`"), 1,
     "single: DEPENDENCIES.md does not mention lib/single.h"),
    ("a longer path is not a mention", lambda r: replace(r, "DEPENDENCIES.md", "`lib/single.h`", "`lib/single.hpp`"),
     1, "single: DEPENDENCIES.md does not mention lib/single.h"),
    ("missing DEPENDENCIES.md fails", lambda r: os.remove(os.path.join(r, "DEPENDENCIES.md")), 1,
     "DEPENDENCIES.md is missing"),
    ("a table row with an old version fails",
     lambda r: replace(r, "DEPENDENCIES.md", "| 1.2.3 (from", "| 1.2.2 (from"), 1,
     "alpha: no table row in DEPENDENCIES.md that names lib/alpha shows the listed version '1.2.3'"),
    ("a longer version is not the listed version",
     lambda r: replace(r, "DEPENDENCIES.md", "| 1.2.3 (from", "| 1.2.3.1 (from"), 1,
     "alpha: no table row in DEPENDENCIES.md that names lib/alpha shows the listed version '1.2.3'"),
    ("a version with more words is not the listed version",
     lambda r: replace(r, "DEPENDENCIES.md", "| 1.2.3 (from", "| 1.2.3 RC1 (from"), 1,
     "alpha: no table row in DEPENDENCIES.md that names lib/alpha shows the listed version '1.2.3'"),
    ("a version outside the table row does not count", move_version_to_prose, 1,
     "alpha: no table row in DEPENDENCIES.md that names lib/alpha shows the listed version '1.2.3'"),
    ("a hand-written version must be in the table too", edit_list(set_single_version), 1,
     "single: no table row in DEPENDENCIES.md that names lib/single.h shows the listed version '2.0'"),
    ("broken list entry fails", edit_list(drop_sha256), 1, "bad list entry: alpha: missing sha256"),
    ("a library listed twice fails", edit_list(list_alpha_twice), 1, "bad list entry: alpha: listed twice"),
    ("a version that is not text fails", edit_list(set_single_version_to_none), 1,
     "bad list entry: single: \"version\" must be text (\"unversioned\" when there is none)"),
    ("a path under two libraries fails", edit_list(share_a_path), 1,
     "bad list entry: gamma: path lib/single.h is listed under two libraries"),
    ("an absolute path fails", edit_list(set_single_path("/lib/single.h")), 1,
     "bad list entry: single: path '/lib/single.h' must be relative to the source root"),
    ("a path that climbs out with .. fails", edit_list(set_single_path("lib/../lib/single.h")), 1,
     "bad list entry: single: path 'lib/../lib/single.h' must be relative to the source root"),
    ("a path with a backslash fails", edit_list(set_single_path("lib\\single.h")), 1,
     "bad list entry: single: path 'lib\\\\single.h' must be relative to the source root, with '/'"),
]


def fresh(tmp, name):
    root = os.path.join(tmp, name.replace(" ", "_"))
    shutil.copytree(TREE, root)
    os.makedirs(os.path.join(root, "tests", "deps"))
    os.replace(os.path.join(root, "third_party.json"), os.path.join(root, LIST))
    return root


def main():
    failed = []
    cases = set()

    def expect(name, ok, detail):
        cases.add(name.split(": ")[0])  # "case: sub-check" counts as one case
        if not ok:
            failed.append(name)
            print(f"test_check_deps: FAIL ({name}): {detail}")

    with tempfile.TemporaryDirectory() as tmp:
        tmp = os.path.realpath(tmp)

        root = fresh(tmp, "fixture hashes")
        for entry in load_list(root)["libraries"]:
            want = documented_hash(root, entry["paths"])
            expect("fixture hashes follow the documented format", entry["sha256"] == want,
                   f"{entry['name']}: {entry['sha256']} != {want}")

        for name, change, code, text in CHECK_CASES:
            root = fresh(tmp, name)
            if change:
                change(root)
            before = read(root, LIST)
            got, out = run(root)
            expect(name, got == code and text in out, f"exit {got} (wanted {code}), output:\n{out}")
            expect(f"{name}: the list is left alone", read(root, LIST) == before, "the check wrote the list")

        name = "--update fixes a hash"
        root = fresh(tmp, name)
        old = lib(load_list(root), "alpha")["sha256"]
        write(root, "lib/alpha/src/a.c", "int alpha_add(int a, int b) { return b + a; }\n")
        new = documented_hash(root, ["lib/alpha"])
        got, out = run(root, "--update")
        expect(name, got == 0 and f"updated alpha: sha256 {old} -> {new}" in out, f"exit {got}, output:\n{out}")
        expect(f"{name}: the list holds the new hash", lib(load_list(root), "alpha")["sha256"] == new,
               "hash not written")
        got, out = run(root)
        expect(f"{name}: the check then passes", got == 0, f"exit {got}, output:\n{out}")

        name = "--update reads a new version"
        root = fresh(tmp, name)
        replace(root, "lib/alpha/alpha.h", "ALPHA_VERSION_PATCH 3", "ALPHA_VERSION_PATCH 4")
        got, out = run(root, "--update")
        expect(name, "updated alpha: version '1.2.3' -> '1.2.4'" in out, f"exit {got}, output:\n{out}")
        expect(f"{name}: the list holds it", lib(load_list(root), "alpha")["version"] == "1.2.4", "not written")
        expect(f"{name}: the old table row still fails", got == 1 and
               "no table row in DEPENDENCIES.md that names lib/alpha shows the listed version '1.2.4'" in out,
               f"exit {got}, output:\n{out}")
        replace(root, "DEPENDENCIES.md", "| 1.2.3 (from", "| 1.2.4 (from")
        got, out = run(root)
        expect(f"{name}: the check passes once the row is updated", got == 0, f"exit {got}, output:\n{out}")

        name = "--update asks for a hand check of a version it cannot read"
        root = fresh(tmp, name)
        write(root, "lib/single.h", "/* single, patched */\n")
        got, out = run(root, "--update")
        expect(name, got == 0 and "single: has no version string, check by hand that 'unversioned' is still right "
               "(in the list and in DEPENDENCIES.md)" in out, f"exit {got}, output:\n{out}")

        name = "--update does not hide an unlisted folder"
        root = fresh(tmp, name)
        write(root, "lib/newlib/new.h", "/* new */\n")
        before = read(root, LIST)
        got, out = run(root, "--update")
        expect(name, got == 1 and "lib/newlib is not in the list" in out, f"exit {got}, output:\n{out}")
        expect(f"{name}: the list is left alone", read(root, LIST) == before, "the list was rewritten")

        name = "--list uses the given list"
        root = fresh(tmp, name)
        data = load_list(root)
        set_list_version(data)
        other = os.path.join(tmp, "other_list.json")
        with open(other, "w", encoding="utf-8") as fh:
            json.dump(data, fh, indent=2)
        before = read(root, LIST)
        got, out = run(root, "--list", other)
        expect(name, got == 1 and "alpha: lib/alpha/alpha.h says version '1.2.3' but the list says '1.2.4'" in out,
               f"exit {got}, output:\n{out}")
        expect(f"{name}: the default list is left alone", read(root, LIST) == before, "the default list changed")
        got, out = run(root, "--list")
        expect(f"{name}: --list without a file prints the usage", got == 1 and "Usage:" in out,
               f"exit {got}, output:\n{out}")

        name = "a missing list fails"
        got, out = run(os.path.join(tmp, "no such tree"))
        expect(name, got == 1 and "cannot read the list" in out, f"exit {got}, output:\n{out}")

        name = "no source root prints the usage"
        got, out = run()
        expect(name, got == 1 and "Usage:" in out, f"exit {got}, output:\n{out}")

    if failed:
        print(f"test_check_deps: FAIL, {len(failed)} case(s)")
        return 1
    print(f"test_check_deps: ok, {len(cases)} cases")
    return 0


if __name__ == "__main__":
    sys.exit(main())
