"""Fork: error handling where SatDump runs work in its own threads, and in the recorder and the
processing handler, which only the GUI runs (so the unit tests cannot reach them).

Fails when one of the files below
  - catches only std::runtime_error (any other error then ended the program),
  - hands an error message to the logger as its format (a '%' in the message broke the log line),
  - has an empty catch block (the error was dropped without a word),
  - locks a mutex by hand and then starts a try block before unlocking it (an error the try does
    not catch leaves the mutex locked, and the next caller waits on it for ever).
Also fails when
  - one of LOCK_GUARD_FILES locks a mutex by hand at all (use std::lock_guard), or
  - one of THREAD_FILES starts a thread or a thread-pool task but has no catch (...) anywhere: an
    error leaving a thread ends the whole program. This is a coarse check (one catch-all per file),
    the unit tests check the behaviour.

Usage: thread_catches.py <source root>
"""

import pathlib
import re
import sys

# The recorder's tracking widget (src-interface/recorder/tracking) is not covered yet
FILES = [
    "src-interface/recorder/*.cpp",
    "src-core/handlers/processing_handler.h",
    "src-cli/legacy/autotrack/autotrack_proc.cpp",
]

THREAD_FILES = [
    "src-core/pipeline/pipeline_run.cpp",
    "src-core/pipeline/live_pipeline.cpp",
    "src-core/handlers/processing_handler.h",
    "src-core/handlers/processing/processing.cpp",
    "src-core/utils/task_queue.cpp",
    "src-core/utils/task_scheduler.cpp",
    "src-core/utils/mqtt_client.cpp",
    "src-core/utils/tasked_button.h",
    "src-core/common/tracking/scheduler/scheduler.cpp",
    "src-core/common/dsp_source_sink/file_source.cpp",
    "src-core/image/image_saving_thread.h",
    "src-interface/recorder/recorder.cpp",
    "src-cli/legacy/autotrack/autotrack_proc.cpp",
]

LOCK_GUARD_FILES = [
    "src-core/common/tracking/scheduler/scheduler.cpp",
    "src-cli/legacy/autotrack/autotrack_proc.cpp",
    "src-core/image/image_saving_thread.h",
]

CHECKS = [
    (re.compile(r"catch\s*\(\s*(const\s+)?std::runtime_error\b"), "catches only std::runtime_error"),
    (re.compile(r"logger->\w+\(\s*e\.what\(\)\s*\)"), "uses an error message as the log format"),
    (re.compile(r"catch\s*\([^)]*\)\s*\{\s*\}"), "has an empty catch block"),
    (
        re.compile(r"\b(\w+)\s*(?:\.|->)\s*lock\(\);(?:(?!\b\1\s*(?:\.|->)\s*unlock\(\)).)*?\btry\b", re.S),
        "locks a mutex by hand, then starts a try block before unlocking it",
    ),
]

MANUAL_LOCK = re.compile(r"\b\w+\s*(?:\.|->)\s*lock\(\);")
THREAD_START = re.compile(r"std::thread\s*\(|std::thread\s+\w+\s*\(|\b(?:\w*pool\w*|tp)\s*(?:\.|->)\s*push\s*\(")
CATCH_ALL = re.compile(r"catch\s*\(\s*\.\.\.\s*\)")


def line_of(text, pos):
    return text.count("\n", 0, pos) + 1


def expand(root, patterns):
    return sorted({f for pattern in patterns for f in root.glob(pattern)})


def main():
    root = pathlib.Path(sys.argv[1])
    files = expand(root, FILES)
    thread_files = expand(root, THREAD_FILES)
    lock_files = expand(root, LOCK_GUARD_FILES)
    if len(files) < len(FILES) or len(thread_files) < len(THREAD_FILES) or len(lock_files) < len(LOCK_GUARD_FILES):
        print("files missing under", root)
        return 1

    found = 0
    for f in sorted(set(files) | set(thread_files)):
        text = f.read_text(encoding="utf-8", errors="replace")
        for regex, what in CHECKS:
            for m in regex.finditer(text):
                print(f"{f.relative_to(root)}:{line_of(text, m.start())}: {what}")
                found += 1
        if f in lock_files:
            for m in MANUAL_LOCK.finditer(text):
                print(f"{f.relative_to(root)}:{line_of(text, m.start())}: locks a mutex by hand (use std::lock_guard)")
                found += 1
        if f in thread_files:
            starts = list(THREAD_START.finditer(text))
            if starts and not CATCH_ALL.search(text):
                print(f"{f.relative_to(root)}:{line_of(text, starts[0].start())}: starts a thread but catches no error that could leave it")
                found += 1
    print(f"{len(set(files) | set(thread_files))} files checked, {found} problems")
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
