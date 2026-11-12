"""Fork: checks that the third-party libraries bundled in the source tree still match the list
in tests/deps/third_party.json, so a library cannot be updated, patched or added without the
list (and DEPENDENCIES.md) being updated with it.

For each library the list records its folders or files, its version and a sha256 hash of its
contents. The check fails, with a plain message, when
  - a listed folder or file is missing,
  - the version written in the library's own files no longer matches the list (or the line
    that holds it can no longer be found),
  - the contents changed (the hash differs),
  - a folder or file appears directly under a scanned folder ("scan": src-core/libs and the
    folders that mix bundled files with others) that no library and no "not_libraries" entry
    covers, or a scanned folder is missing,
  - DEPENDENCIES.md does not mention a listed library's first path, or no table row that names
    that path shows the listed version: one cell must be the version, alone or followed by
    " (", ", " or "; " and more text.

The hash covers every file under the library's paths, sorted by their path relative to the
source root: for each file its path, its size and its bytes. Only .DS_Store and Thumbs.db are
skipped. Libraries without a version string are tracked by the hash alone and their version
is written in the list by hand ("version_from": null).

Usage:
  check_deps.py <source root>                  check (exit 0 when everything matches)
  check_deps.py <source root> --update         rewrite the hashes, and the versions it can read,
                                               printing every change (run it on purpose, like
                                               the warning ratchet's --update)
  check_deps.py <source root> --list <file>    use another list (default
                                               <source root>/tests/deps/third_party.json)
"""
import hashlib
import json
import os
import re
import sys

SKIP_FILES = {".DS_Store", "Thumbs.db"}
REQUIRED_KEYS = ("name", "paths", "version", "version_from", "sha256")


def say(msg):
    print(f"check_deps: {msg}")


def safe_rel(path):
    """True when a list path is relative, uses '/' and stays inside the source root."""
    if not isinstance(path, str) or not path or path.startswith("/") or "\\" in path:
        return False
    return all(part not in ("", ".", "..") for part in path.split("/"))


def files_under(root, rel):
    """Every file under a listed path, as POSIX paths relative to the source root."""
    full = os.path.join(root, *rel.split("/"))
    if os.path.isfile(full):
        return [rel]
    found = []
    for dirpath, dirnames, filenames in os.walk(full):
        dirnames.sort()
        for name in filenames:
            if name in SKIP_FILES:
                continue
            path = os.path.join(dirpath, name)
            found.append(os.path.relpath(path, root).replace(os.sep, "/"))
    return found


def content_hash(root, paths):
    files = set()
    for rel in paths:
        files.update(files_under(root, rel))
    digest = hashlib.sha256()
    for rel in sorted(files):
        with open(os.path.join(root, *rel.split("/")), "rb") as fh:
            data = fh.read()
        digest.update(rel.encode("utf-8") + b"\0" + str(len(data)).encode("ascii") + b"\0")
        digest.update(data)
    return digest.hexdigest()


def read_version(root, spec):
    """Version text read with spec {"file", "regex"[, "format"]}, or None when the regex no
    longer matches. Several groups are joined with '.' unless a format such as "{0}.{1}{2}" is
    given."""
    path = os.path.join(root, *spec["file"].split("/"))
    with open(path, encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    m = re.search(spec["regex"], text, re.S)
    if not m:
        return None
    groups = [(g or "").strip() for g in m.groups()] if m.groups() else [m.group(0).strip()]
    if "format" in spec:
        return spec["format"].format(*groups)
    return ".".join(groups)


def check_shape(data):
    """Plain messages for a list that cannot be checked at all."""
    problems = []
    if not isinstance(data, dict) or not isinstance(data.get("libraries"), list):
        return ["the list has no \"libraries\" array"]
    for key in ("scan", "not_libraries"):
        if not isinstance(data.get(key, []), list):
            return [f"\"{key}\" must be an array"]
    for scan in data.get("scan", []):
        if not safe_rel(scan):
            problems.append(f"scanned folder {scan!r} must be relative to the source root, with '/'")
    for entry in data.get("not_libraries", []):
        if not isinstance(entry, dict) or not safe_rel(entry.get("path")):
            problems.append(f"not_libraries entry {entry!r} needs a relative \"path\"")
    names = set()
    paths = set()
    for i, lib in enumerate(data["libraries"]):
        label = lib.get("name", f"entry {i + 1}") if isinstance(lib, dict) else f"entry {i + 1}"
        if not isinstance(lib, dict):
            problems.append(f"{label}: is not an object")
            continue
        missing = [k for k in REQUIRED_KEYS if k not in lib]
        if missing:
            problems.append(f"{label}: missing {', '.join(missing)}")
            continue
        if not isinstance(lib["version"], str) or not lib["version"].strip():
            problems.append(f"{label}: \"version\" must be text (\"unversioned\" when there is none)")
        if lib["name"] in names:
            problems.append(f"{label}: listed twice")
        names.add(lib["name"])
        if not isinstance(lib["paths"], list) or not lib["paths"]:
            problems.append(f"{label}: \"paths\" must be a non-empty array")
            continue
        for rel in lib["paths"]:
            if not safe_rel(rel):
                problems.append(f"{label}: path {rel!r} must be relative to the source root, with '/'")
            elif rel in paths:
                problems.append(f"{label}: path {rel} is listed under two libraries")
            paths.add(rel)
        spec = lib["version_from"]
        if spec is not None:
            if not isinstance(spec, dict) or not safe_rel(spec.get("file")) or not spec.get("regex"):
                problems.append(f"{label}: \"version_from\" needs a \"file\" and a \"regex\" (or null)")
            else:
                try:
                    re.compile(spec["regex"])
                except re.error as err:
                    problems.append(f"{label}: the version regex does not compile ({err})")
    return problems


def unlisted(root, data):
    """Messages for scanned folders that are missing and for their direct children that no
    library or not_libraries entry covers."""
    covered = [p for lib in data["libraries"] for p in lib["paths"]]
    covered += [e["path"] for e in data.get("not_libraries", [])]
    found = []
    for scan in data.get("scan", []):
        full = os.path.join(root, *scan.split("/"))
        if not os.path.isdir(full):
            found.append(f"the scanned folder {scan} is missing (moved or deleted? fix \"scan\" "
                         f"in the list by hand)")
            continue
        for name in sorted(os.listdir(full)):
            if name in SKIP_FILES:
                continue
            child = f"{scan}/{name}"
            if not any(p == child or p.startswith(child + "/") for p in covered):
                found.append(f"{child} is not in the list: add it to tests/deps/third_party.json "
                             f"(a library, or a not_libraries entry for the project's own file) "
                             f"and DEPENDENCIES.md")
    return found


def mentions(path):
    """Regex for a whole mention of a list path: src-core/libs/jpeg12 does not mention
    src-core/libs/jpeg."""
    return re.compile(r"(?<![\w./-])" + re.escape(path) + r"(?![\w.-])")


def shows_version(row, version):
    """True when a cell of the Markdown table row is the version, alone or followed by " (",
    ", " or "; " and more text (so '1.3.0 RC1' does not show version '1.3.0')."""
    cell = re.compile(re.escape(version) + r"(?:$|\s\(|,\s|;\s)")
    return any(cell.match(c.strip()) for c in row.strip().strip("|").split("|"))


def doc_problems(doc, doc_text, libraries):
    """Messages for listed libraries the document does not mention, or whose table row does
    not show the listed version."""
    found = []
    rows = [line for line in doc_text.splitlines() if line.lstrip().startswith("|")]
    for lib in libraries:
        first = lib["paths"][0]
        mention = mentions(first)
        if not mention.search(doc_text):
            found.append(f"{lib['name']}: {doc} does not mention {first}")
            continue
        named = [row for row in rows if mention.search(row)]
        if not any(shows_version(row, lib["version"]) for row in named):
            found.append(f"{lib['name']}: no table row in {doc} that names {first} shows the "
                         f"listed version {lib['version']!r} (update the row by hand)")
    return found


def main():
    args = sys.argv[1:]
    update = "--update" in args
    args = [a for a in args if a != "--update"]
    list_path = None
    if "--list" in args:
        at = args.index("--list")
        if at + 1 >= len(args):
            print(__doc__)
            return 1
        list_path = args[at + 1]
        del args[at:at + 2]
    if len(args) != 1 or args[0].startswith("-"):
        print(__doc__)
        return 1
    root = os.path.realpath(args[0])
    if list_path is None:
        list_path = os.path.join(root, "tests", "deps", "third_party.json")
    try:
        with open(list_path, encoding="utf-8") as fh:
            data = json.load(fh)
    except (OSError, ValueError) as err:
        say(f"FAIL, cannot read the list {list_path}: {err}")
        return 1
    shape = check_shape(data)
    if shape:
        for msg in shape:
            say(f"FAIL, bad list entry: {msg}")
        return 1

    failures = []
    changes = []
    reminders = []
    fixable = 0  # failures that --update can fix (a changed hash or a readable new version)
    for lib in data["libraries"]:
        name = lib["name"]
        missing = [p for p in lib["paths"] if not os.path.exists(os.path.join(root, *p.split("/")))]
        if missing:
            for rel in missing:
                failures.append(f"{name}: {rel} is missing (moved or deleted? fix the list by hand)")
            continue
        spec = lib["version_from"]
        if spec is not None:
            if not os.path.isfile(os.path.join(root, *spec["file"].split("/"))):
                failures.append(f"{name}: the version file {spec['file']} is missing")
            else:
                got = read_version(root, spec)
                if got is None:
                    failures.append(f"{name}: the version line was not found in {spec['file']} "
                                    f"(the file changed shape; fix \"version_from\" by hand)")
                elif got != lib["version"]:
                    if update:
                        changes.append(f"{name}: version {lib['version']!r} -> {got!r}")
                        lib["version"] = got
                    else:
                        failures.append(f"{name}: {spec['file']} says version {got!r} but the list "
                                        f"says {lib['version']!r}")
                        fixable += 1
        digest = content_hash(root, lib["paths"])
        if digest != lib["sha256"]:
            if update:
                changes.append(f"{name}: sha256 {lib['sha256'] or '(none)'} -> {digest}")
                if spec is None and lib["sha256"]:
                    reminders.append(f"{name}: has no version string, check by hand that "
                                     f"{lib['version']!r} is still right (in the list and in "
                                     f"{data.get('doc') or 'the documentation'})")
                lib["sha256"] = digest
            else:
                failures.append(f"{name}: the files changed (sha256 {digest} is not the listed "
                                f"{lib['sha256'] or '(none)'})")
                fixable += 1

    failures += unlisted(root, data)

    doc = data.get("doc")
    if doc:
        doc_path = os.path.join(root, *doc.split("/"))
        try:
            with open(doc_path, encoding="utf-8") as fh:
                doc_text = fh.read()
        except OSError:
            failures.append(f"{doc} is missing")
        else:
            failures += doc_problems(doc, doc_text, data["libraries"])

    for msg in changes:
        say(f"updated {msg}")
    for msg in reminders:
        say(msg)
    if update and changes:
        with open(list_path, "w", encoding="utf-8") as fh:
            json.dump(data, fh, indent=2, ensure_ascii=False)
            fh.write("\n")
        say(f"wrote {len(changes)} change(s) to {list_path}")
    for msg in failures:
        say(f"FAIL, {msg}")
    if failures:
        hint = " (after a deliberate library update, run with --update)" if fixable else ""
        say(f"FAIL, {len(failures)} problem(s){hint}")
        return 1
    say(f"ok, {len(data['libraries'])} libraries match the list")
    return 0


if __name__ == "__main__":
    sys.exit(main())
