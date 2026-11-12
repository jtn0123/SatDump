"""Self-test for ratchet.py: a warning gives the same key whether the compiler printed its
path absolute or relative to the build folder (ccache's base_dir makes paths relative).

Usage: test_ratchet.py   (exit 0 when it passes)"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ratchet  # noqa: E402


def keys(root, build, text):
    log = os.path.join(build, "gate-build.log")
    with open(log, "w") as fh:
        fh.write(text)
    return dict(ratchet.keys_from_log(log, root))


def main():
    with tempfile.TemporaryDirectory() as tmp:
        root = os.path.realpath(tmp)
        build = os.path.join(root, "build-pi")
        os.makedirs(build)
        src = os.path.join(root, "src-core", "a.cpp")
        warn = ":12:3: warning: unused variable 'x' [-Wunused-variable]\n"
        want = {"src-core/a.cpp -Wunused-variable": 1}
        cases = {
            "absolute": src + warn,
            "relative to the build folder": "../src-core/a.cpp" + warn,
            "relative with a detour": "../plugins/../src-core/a.cpp" + warn,
        }
        failed = 0
        for name, text in cases.items():
            got = keys(root, build, text)
            if got != want:
                print(f"test_ratchet: FAIL ({name}): {got} != {want}")
                failed += 1
        both = keys(root, build, cases["absolute"] + cases["relative to the build folder"])
        if both != want:  # the same warning seen twice counts once
            print(f"test_ratchet: FAIL (same warning twice): {both} != {want}")
            failed += 1
    if failed:
        return 1
    print("test_ratchet: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
