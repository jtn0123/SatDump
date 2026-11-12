# About this fork

This is an **unofficial fork** of [SatDump](https://github.com/SatDump/SatDump). It is
not affiliated with or endorsed by the SatDump project. Licence: GPL-3.0, like upstream.

Branch `pi` holds the fork's changes on top of upstream `master`. The fork targets a
Raspberry Pi 5 (Debian 13) that receives weather satellites (MetOp, Meteor). Goals:

- a softer, rounder look (the "soft skin") with 13 extra themes; the default is "Soft OLED"
- safety: tests, crash fixes, predictable error handling and exit codes
- current dependencies
- Rust for the parts that read raw radio data, behind a build switch that is off by default

## Changes from upstream

Every change is a separate commit on `pi` (`git log upstream/master..pi`). Summary:

| Area | Change | First added |
|---|---|---|
| Crash fix | MetOp/NOAA: skip MHS calibration when fewer than 6 lines were decoded (out-of-bounds read) | 2026-11-11 |
| Crash fix | Reed-Solomon with "no fill" (Meteor LRPT) wrote one byte past each codeword | 2026-11-11 |
| Crash fix | MetOp / MetOp-SG admin messages: short packets and unexpected XML are skipped | 2026-11-11 |
| Crash fix | CCSDS CRC and time stamps: short packets are rejected; time no longer wraps in 32 bits | 2026-11-11 |
| Crash fix | MetOp, NOAA, Meteor: a cut-off last frame is dropped instead of decoded from stale bytes | 2026-11-11 |
| Crash fix | A pass with no data (empty or very short file) finishes with exit 0 instead of failing | 2026-11-11 |
| Crash fix | Meteor LRPT: the picture decompression shifted negative numbers left (undefined behaviour), found by fuzzing | 2026-11-11 |
| Crash fix | Meteor LRPT: a short (damaged) telemetry packet was read past its end and put the calibration of every later line out of step | 2026-11-11 |
| Crash fix | Images: white balance and equalize no longer read past an empty image; copying an empty image is safe; a flat channel is no longer turned black | 2026-11-11 |
| Start-up | A damaged settings file no longer stops SatDump: it uses the default settings and leaves the file as it is | 2026-11-11 |
| Start-up | A damaged database (`main.db`) no longer stops SatDump: that run keeps its data in memory | 2026-11-11 |
| Start-up | HOME not set (services, cron) no longer crashes: settings go under `XDG_CONFIG_HOME`, then the user's home folder | 2026-11-11 |
| Start-up | A crash ends with its real signal (so a service manager sees it), and an error that nothing handled is printed before the stop | 2026-11-11 |
| Bug fix | Background updates (orbit data, Earth-orientation data) waited for the task listed last, not the one due first; a "never" interval no longer overflows | 2026-11-11 |
| Bug fix | Memory leaks: picture metadata whenever a picture was reassigned, every composite picture (and all of it after an expression error), and the old database when a damaged one is replaced | 2026-11-11 |
| Bug fix | CCSDS AOS packet splitter: a packet cut short by a new packet header (damaged frame) is dropped instead of being passed on cut off, one byte of the next header included, or empty | 2026-11-11 |
| Bug fix | CCSDS AOS packet splitter: after a packet header cut across two frames, a damaged frame could make it write past the end of the packet, lose that packet and put its bytes in front of the next one | 2026-11-11 |
| Bug fix | CCSDS AOS packet splitter: in the MetOp decoder, a lost frame (found from the virtual channel's frame counter) drops the packet in progress; in every AOS decoder, a frame with an impossible packet pointer does too. Before, that packet was finished with bytes from a later frame | 2026-11-11 |
| Bug fix | AMSU: channel 2 (AMSU-A2) took its low byte from the high byte, so every value was wrong; a damaged MetOp AMSU packet that is too short after the fill words are removed is dropped instead of read past its end | 2026-11-11 |
| Bug fix | MetOp GOME: band 1 (365 channels) was never filled, the first channel of each later band was blank, and one channel was read past the end of an array | 2026-11-11 |
| Crash fix | MetOp-A or an unknown MetOp: the MHS telemetry no longer stops the instruments step for lack of calibration coefficients (AMSU, GOME and the rest were lost); it is saved without temperatures and currents | 2026-11-11 |
| Crash fix | NOAA HIRS: every HIRS frame read one byte past its buffer while the 13-bit words were unpacked | 2026-11-11 |
| Crash fix | NOAA HIRS calibration: a calibration sequence on the first or last line, or a pass in which one of the thermometers (PRTs) has no reading, no longer reads outside the readings (or crashes); the nearest reading is used, and when a thermometer has no reading at all the lines are left uncalibrated | 2026-11-11 |
| Bug fix | NOAA HIRS: averaging samples that are all 0 (dropped frames) gives 0 instead of a NaN converted to an integer (undefined behaviour) | 2026-11-11 |
| Bug fix | MetOp IASI imager: a pixel whose warm and cold calibration views read the same is now 0 (no data) instead of white or an undefined value | 2026-11-11 |
| Bug fix | MetOp AHRPT, MetOp X-band dump and Meteor LRPT decoders: error counts, lock state, correlation and the plots start at 0, so stats read before the first frame (live mode) are no longer garbage and a Meteor M2-x decoder no longer reports SYNCED before it has any data | 2026-11-11 |
| Bug fix | Meteor LRPT: picture segments that were never received, or that had no picture data, no longer leave their header values unset (the validity check read them) | 2026-11-11 |
| Crash fix | A new-DSP block whose work() throws no longer ends SatDump: the error is logged with the block ID, the blocks after it end as at the end of a stream, and the blocks before it are never left waiting for room | 2026-11-11 |
| Crash fix | Destroying a new-DSP block whose thread already ended (end of stream or failure) no longer stops SatDump; destroying one that is still running names the block in the log before stopping | 2026-11-11 |
| Crash fix | A legacy DSP block whose work() throws no longer ends SatDump: the error is logged with the block's class name, the blocks after it wind down, and a block that is started again starts clean | 2026-11-11 |
| Bug fix | When a block of a flowgraph or demodulator fails to start, the blocks already started are stopped again instead of being left running (destroying them later stopped SatDump) | 2026-11-11 |
| Bug fix | Blocks with several inputs (Add, Multiply, selector and others) keep taking their other inputs after one of them ends, so the blocks feeding those inputs no longer hang (for example when one branch before an Add failed) | 2026-11-11 |
| CLI | A command during which a DSP block failed now ends with "Error running command" and exit code 2, as its output is incomplete (before the fork it crashed) | 2026-11-11 |
| Crash fix | A flowgraph block whose start() throws something unusual, or whose stop() throws, is logged instead of ending SatDump | 2026-11-11 |
| Bug fix | The network, BladeRF and HackRF sinks treat "no data" (-1) from their input as no data instead of a huge sample count | 2026-11-11 |
| Bug fix | Two stop flags shared between threads (the old-to-new DSP bridge and the reshape buffer) are now atomic, removing a data race | 2026-11-11 |
| Bug fix | A new-DSP setting that cannot be applied is logged with the block and setting name instead of being dropped silently | 2026-11-11 |
| Build | On GCC, a throw that can only end in std::terminate (for example in a destructor) is now a build error | 2026-11-11 |
| Tests | Unit tests for DSP blocks that fail, fail to start, end, or are destroyed, for multi-input blocks whose inputs end at different times, and for the CLI exit code after a block failure | 2026-11-11 |
| Build | Warnings that are real bugs now stop the build: a function that can end without returning its value, a shift past the width of its type, a switch on a bool, an ignored result that must be used, and a pointer into a packed struct that may be unaligned (each added only if the compiler knows the flag) | 2026-11-11 |
| Build | RTL-TCP client: the result of sending a command to the server was thrown away, which stops the build with GCC's _FORTIFY_SOURCE (on by default on Ubuntu) now that ignored results are errors; a failed or short send is now logged as a warning | 2026-11-11 |
| Build | ImGui and RFNM headers no longer define the same macro twice (IMGUI_DEFINE_MATH_OPERATORS, RFNM_PACKED_STRUCT), which gave "redefined" warnings | 2026-11-11 |
| Bug fix | Shapefile map layer: points were plotted through pointers into a packed struct that may be unaligned; they are now copied first | 2026-11-11 |
| Build | LEO channel model: a switch on a bool (with a default case that could never run) is now a plain if | 2026-11-11 |
| Build | Channel model and Aaronia sources use VOLK's current scalar multiply (the old one is deprecated since VOLK 3.1); the output is bit-identical | 2026-11-11 |
| Build | Viterbi decoder: the deprecated work() with an unused size argument is removed and its callers (generic punctured Viterbi, DVB-S) use the normal one; decoding is byte-for-byte the same | 2026-11-11 |
| Bug fix | ELEKTRO/Arktika GGAK merger: frames recovered after two arrived out of order were never removed from its queue, so it stayed in recovery and about 20 frames later dropped a good frame; it now goes back to normal once the missing frame arrives | 2026-11-11 |
| Bug fix | ELEKTRO-L MSU-GS: the line time stamp lost its top byte (a shift past 32 bits, undefined behaviour) and became huge once byte 11 reached 0x80; it is now the full 40-bit count (no product uses these times yet). Each visible line also read 35 bytes past the end of its frame, into values never used; pictures are unchanged | 2026-11-11 |
| Bug fix | BitView tools: temporary files are made safely (mkstemp, the name and the file in one step) instead of with tmpnam, whose name another program could take first | 2026-11-11 |
| Build | RTL-SDR device list: two lines built a second string after a comma and threw it away; removed so clang builds with the new error flags (device names unchanged) | 2026-11-11 |
| Crash fix | `satdump pipeline`: when a module fails, SatDump no longer aborts. The failed module is stopped first, so a demodulator's running DSP blocks are not destroyed while still running (that ended in "pure virtual method called"). When two modules run at the same time, the other one is stopped too, including one waiting on a full buffer. The error is logged as "Error running command" and the command exits with code 2. The module list shown in the GUI is cleared on failure too | 2026-11-11 |
| Crash fix | Live decoding: a module that fails is logged and stopped, and the data feeding it is let through, so the recording and the other modules keep going. Stopping never throws and never waits for ever. A pipeline whose modules cannot all be set up starts none of them, and one without live modules is refused | 2026-11-11 |
| CLI | `satdump legacy live`: a module that failed during the run is reported once everything has stopped, and the command exits with code 2 (it used to exit 0, or hang). An error once the radio runs, such as --http_server given as a number, now stops the radio, the FFT and the pipelines and is reported with exit code 2. It used to hang for ever, past --timeout. A pipeline that cannot start now stops the FFT blocks it started. In multi-VFO mode, a VFO that cannot start stops the VFOs and radio already started before the error is reported (SatDump used to crash or hang) | 2026-11-11 |
| Bug fix | Buffers between modules: stopping a reader or writer that is just about to wait now always wakes it. The wake-up could be lost, and stopping then hung | 2026-11-11 |
| Crash fix | GUI processing: a run that fails in the background is logged instead of closing SatDump. The busy flag is cleared so the controls come back. The offline processing window always reports that processing ended, also when the output folder cannot be made. Waiting for a previous run that never started is no longer a silently ignored error | 2026-11-11 |
| Crash fix | Recorder: errors from opening or starting a radio, or from starting processing, are caught whatever their type (only runtime_error was caught, and anything else closed SatDump). The error text is logged as text, so a '%' in it no longer breaks the log line. A samplerate that cannot be restored is now logged instead of ignored. An error while stopping is logged instead of lost, and the live pipeline is always released. The VFO list lock is let go on every way out | 2026-11-11 |
| Crash fix | Background task queue: a task that throws something other than a std::exception no longer closes SatDump, and failures are logged instead of silently dropped. A task pushed just as the queue's thread was ending is no longer left unrun | 2026-11-11 |
| Crash fix | Autotrack: a callback or event handler that throws at AOS, LOS or engage is logged and autotrack carries on. It used to close SatDump and leave its lock taken. Its lock is now let go on every way out (new TLEs, setting the station, engaging, choosing what to track). When no pass follows the last one, nothing is engaged on a pass that does not exist (this read past the end of the list). In multi mode each AOS/LOS event now carries its own pass instead of the first one in the list | 2026-11-11 |
| Crash fix | Scheduled tasks (Kepler, IERS and GOES DCP updates): a task that fails is logged and runs again at its next time. The error used to close SatDump, also during a pass | 2026-11-11 |
| Crash fix | `satdump legacy autotrack`: a live pipeline that cannot start at AOS is logged whatever the error, and its lock is always let go. An error other than runtime_error used to leave it taken, so the next pass waited for ever. A failure in the processing after a pass is logged instead of lost | 2026-11-11 |
| Crash fix | Background threads that save images (GOES GRB), read a baseband file, handle MQTT messages, or run a GUI button's task: an error is logged instead of closing SatDump. The image-saving queue is only read under its lock | 2026-11-11 |
| Look | Optional soft skin (`src-core/imgui/imgui_soft.h`), switched on by a theme's `"Soft"` block | 2026-11-11 |
| Look | 13 soft themes, the Inter font, and their generator (`tools/theme_gen`) | 2026-11-11 |
| Look | Default theme "Soft OLED"; a missing theme no longer overwrites the saved choice with "Dark" | 2026-11-11 |
| CLI | Exit codes: 0 success, 1 usage or configuration error, 2 the command failed, 3 radio problem | 2026-11-11 |
| CLI | `pipeline` needs a pipeline name and an input file that exists (else exit 1) | 2026-11-11 |
| Settings | `iers_auto_update` can switch off the Earth-orientation downloads at launch | 2026-11-11 |
| Build | `SATDUMP_CPU_FLAGS` replaces the hard-wired `-march=native`; warnings are no longer dropped | 2026-11-11 |
| Build | `cmake/fork/pi.cmake` preset: all decoders, NEON, no network radio server (it has no password) | 2026-11-11 |
| Build | Linux installs are relocatable (`SATDUMP_RELOCATABLE`, on in the preset): they find their files and libraries wherever they are unpacked | 2026-11-11 |
| Security | The experimental TestRemoteRec server listens on 127.0.0.1 only | 2026-11-11 |
| Tests | `tests/`: unit and CLI tests, a compiler-warning ratchet, and `tests/gate.sh` | 2026-11-11 |
| Tests | Golden tests: synthetic MetOp, NOAA and Meteor recordings (`tests/golden/make_recordings.py`) must give exactly the known-good pictures | 2026-11-11 |
| Tests | Sanitizer build (`SATDUMP_SANITIZERS`): the same tests with memory, leak, index and undefined-behaviour checks | 2026-11-11 |
| Rust | `SATDUMP_WITH_RUST` (off by default): the Rust crate in `rust/`, linked into the core library with its symbols hidden; `SATDUMP_NO_RUST=1` switches it off at run time | 2026-11-11 |
| Rust | First Rust part: the CCSDS derandomiser, tested byte for byte against the C++ one | 2026-11-11 |
| Tests | Fuzzing (`SATDUMP_FUZZ`, clang): random input for the MetOp/Meteor packet readers, the admin messages and Reed-Solomon | 2026-11-11 |
| Tests | `tests/guard.sh`: on a receiving station the test gate never starts during a pass or when the disk is low | 2026-11-11 |
| CI | Upstream's multi-platform build runs only when started by hand | 2026-11-11 |
| CI | `fork_tests.yml`: the test gate, normal and sanitizer builds, on every push (arm64, Debian 13) | 2026-11-11 |

## Exit codes (`satdump` command line)

| Code | Meaning |
|---|---|
| 0 | Finished (a pass with no usable data also finishes, with empty products) |
| 1 | Usage error: unknown option, missing argument, unknown pipeline, input file not found |
| 2 | The command ran and failed (the log line `Error running command` says why) |
| 3 | Radio problem: the radio is not connected, its driver is missing, or it could not be opened, set up or started (`legacy record` and `legacy live`) |

A crash ends the way the system reports crashes: by its signal (a shell shows 128 + the
signal number, for example 139 for a segmentation fault or 134 for an abort), so a service
manager can tell a crash from an ordinary failure.

## Building and testing

On the Pi, or on any arm64 Debian 13 machine (a VM is much faster than the Pi):

```bash
cmake -C cmake/fork/pi.cmake -DSATDUMP_TESTS=ON -S . -B build -G Ninja
tests/gate.sh build            # build, warning check, install into build/check-prefix, all tests
cmake --install build --prefix ~/satdump    # a relocatable install
```

`tests/gate.sh` fails on any test failure and on any **new** compiler warning. When
warnings are fixed, `tests/gate.sh build --update` lowers the baseline in
`tests/warnings/<arch>.txt`; the baseline should only ever go down. The tests never
contact the network.

The golden test decodes three small synthetic recordings (a MetOp-B, a NOAA-19 and a
Meteor-M N2-3 pass, generated on the fly from fixed seeds; nothing in them is received data)
and compares a fingerprint of every picture with `tests/golden/expected-<arch>.txt`. The
Meteor one is compressed by a small JPEG-style encoder in the generator (with its own copy
of the standard tables) and has a lost packet, a damaged packet and short telemetry in it,
so the decoder's repair paths are checked as well. When a change is
meant to alter the pictures, look at them first, then record the new fingerprints with
`tests/golden/golden.sh build/check-prefix --update`.

A second build folder runs the same tests under AddressSanitizer and UBSan, which stop on
any out-of-bounds read or write, use of freed memory, memory leak, or undefined behaviour (exit code 86).
It also turns on the C++ library's own index checks (`_GLIBCXX_ASSERTIONS`), which stop on a
read just past the end of a list even when AddressSanitizer would allow it:

```bash
cmake -C cmake/fork/pi.cmake -DSATDUMP_TESTS=ON -DSATDUMP_SANITIZERS=address,undefined \
      -DSATDUMP_CPU_FLAGS= -DBUILD_GUI=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo -S . -B build-asan -G Ninja
tests/gate.sh build-asan       # the warning check is skipped for this build
```

Sanitizer builds are for testing only: they are slower and use more memory. Memory that
SatDump keeps on purpose until it exits is listed, with the reason, in
`tests/sanitizers/lsan.supp` (one entry today); anything else that leaks fails the tests.

Fuzzing feeds the packet readers millions of random inputs and stops at the first memory
error or undefined behaviour. It needs clang:

```bash
cmake -C cmake/fork/pi.cmake -DSATDUMP_TESTS=ON -DSATDUMP_FUZZ=ON -DSATDUMP_SANITIZERS=address,undefined \
      -DSATDUMP_CPU_FLAGS= -DBUILD_GUI=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -S . -B build-fuzz -G Ninja
cmake --build build-fuzz && ctest --test-dir build-fuzz -L fuzz      # a short, repeatable run of each
tests/fuzz/run.sh build-fuzz/fuzz_msumr_lrpt 600                     # 10 minutes of one fuzzer
```

A long run keeps what it learned in `tests/fuzz/corpus/<name>` (not committed) and writes
any input that fails to `crash-*` in the current folder.

On a receiving station `tests/gate.sh` refuses to start while a pass is scheduled or running
(the lock file `$SATDUMP_PASS_LOCK`, default `$XDG_RUNTIME_DIR/satdump-pass.lock`, or any
running `satdump` process) and when less than `SATDUMP_MIN_FREE_GB` (default 4) GB is free.
It runs at the lowest CPU and disk priority, caps ccache at 2 GB, and writes its process id
to `$XDG_RUNTIME_DIR/satdump-gate.pid` so a pass scheduler can stop it. `SATDUMP_GUARD=off`
skips the two checks on machines that never receive passes (CI does this).

## Rust parts

Rust is off by default; the Pi build leaves it off. To build with it (Debian 13's `cargo`,
`rustc` and `rust-clippy`, Rust 1.85; the crate has no dependencies, so it builds offline):

```bash
cmake -S . -B build -DSATDUMP_WITH_RUST=ON     # added to any of the configurations above
tests/gate.sh build                            # adds the Rust tests below
```

How it fits together:

- `rust/` is one crate, built by cargo from `cmake/fork/rust.cmake` into a static library
  inside `libsatdump_core.so`. Its symbols are hidden there, so plugins reach Rust only
  through the C++ functions in `rust/bridge/rust_bridge.h`.
- Rules for every Rust function C++ calls (`rust/src/ffi.rs`): a panic is caught and comes
  back as a status code; arguments are checked before anything is changed, so on any error
  the buffer is untouched and the C++ code does the work instead; a Rust failure is logged
  once and switches Rust off for the rest of the run.
- `SATDUMP_NO_RUST=1` in the environment switches Rust off; the log says
  `Rust parts: on` or `off` at start-up.
- Each Rust part keeps its C++ twin (for example `derand_ccsds_cpp`) as the reference: the
  unit tests compare them byte for byte, and the golden pictures are checked with Rust on and
  off. Rust files stay at 500 lines or fewer.

Rust tests in the gate: `rust_unit` (cargo test), `rust_clippy` (clippy, warnings are
errors), `cli_rust` (no Rust symbols exported, no extra libraries needed, the switch works),
`golden_no_rust`, and the C++-against-Rust cases in `unit`.

## Known issues left as they are

- When a pipeline runs the demodulator and decoder together (any `baseband` input), the
  hand-off between them at the end of the file drops the last few dozen frames (about
  0.1 s of signal); the exact number changes from run to run. This is upstream behaviour;
  the products of the test recordings are not affected.
- `crcCheckHLDC32` (GK-2A only) compares with `crc ^ 0xFFFFFFFF == crc2`, which C++
  reads as `crc ^ (0xFFFFFFFF == crc2)`. Fixing it safely needs GK-2A recordings to test with.
- The Meteor LRPT decoder does not yet use the frame counter to notice lost frames (the
  MetOp decoder does): a packet that spans a lost frame can still be finished with bytes from
  a later one. It stays off until it has been checked on a real Meteor recording.

## Staying current

`pi` is rebased onto upstream `master` about once a month, and sooner for upstream crash
or security fixes. Small, general fixes may be offered upstream as separate pull requests.
