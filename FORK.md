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
| Look | Optional soft skin (`src-core/imgui/imgui_soft.h`), switched on by a theme's `"Soft"` block | 2026-11-11 |
| Look | 13 soft themes, the Inter font, and their generator (`tools/theme_gen`) | 2026-11-11 |
| Look | Default theme "Soft OLED"; a missing theme no longer overwrites the saved choice with "Dark" | 2026-11-11 |
| CLI | Exit codes: 0 success, 1 usage or configuration error, 2 the command failed | 2026-11-11 |
| CI | Upstream's multi-platform build runs only when started by hand | 2026-11-11 |

## Staying current

`pi` is rebased onto upstream `master` about once a month, and sooner for upstream crash
or security fixes. Small, general fixes may be offered upstream as separate pull requests.
