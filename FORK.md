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
| Look | Optional soft skin (`src-core/imgui/imgui_soft.h`), switched on by a theme's `"Soft"` block | 2026-11-11 |
| Look | 13 soft themes, the Inter font, and their generator (`tools/theme_gen`) | 2026-11-11 |
| Look | Default theme "Soft OLED"; a missing theme no longer overwrites the saved choice with "Dark" | 2026-11-11 |
| CLI | Exit codes: 0 success, 1 usage or configuration error, 2 the command failed | 2026-11-11 |
| CLI | `pipeline` needs a pipeline name and an input file that exists (else exit 1) | 2026-11-11 |
| Settings | `iers_auto_update` can switch off the Earth-orientation downloads at launch | 2026-11-11 |
| Build | `SATDUMP_CPU_FLAGS` replaces the hard-wired `-march=native`; warnings are no longer dropped | 2026-11-11 |
| Build | `cmake/fork/pi.cmake` preset: all decoders, NEON, no network radio server (it has no password) | 2026-11-11 |
| Build | Linux installs are relocatable (`SATDUMP_RELOCATABLE`, on in the preset): they find their files and libraries wherever they are unpacked | 2026-11-11 |
| Security | The experimental TestRemoteRec server listens on 127.0.0.1 only | 2026-11-11 |
| Tests | `tests/`: unit and CLI tests, a compiler-warning ratchet, and `tests/gate.sh` | 2026-11-11 |
| CI | Upstream's multi-platform build runs only when started by hand | 2026-11-11 |

## Exit codes (`satdump` command line)

| Code | Meaning |
|---|---|
| 0 | Finished (a pass with no usable data also finishes, with empty products) |
| 1 | Usage error: unknown option, missing argument, unknown pipeline, input file not found |
| 2 | The command ran and failed (the log line `Error running command` says why) |

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

## Known issues left as they are

- When a pipeline runs the demodulator and decoder together (any `baseband` input), the
  hand-off between them at the end of the file drops the last few dozen frames (about
  0.1 s of signal); the exact number changes from run to run. This is upstream behaviour;
  the products of the test recordings are not affected.
- `crcCheckHLDC32` (GK-2A only) compares with `crc ^ 0xFFFFFFFF == crc2`, which C++
  reads as `crc ^ (0xFFFFFFFF == crc2)`. Fixing it safely needs GK-2A recordings to test with.

## Staying current

`pi` is rebased onto upstream `master` about once a month, and sooner for upstream crash
or security fixes. Small, general fixes may be offered upstream as separate pull requests.
