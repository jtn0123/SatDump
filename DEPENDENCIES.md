# Third-party code in this fork

Fork file. Checked against Debian 13 (trixie) and the Debian security tracker on 2026-11-11.

## In plain English

SatDump keeps copies of nearly seventy small libraries written by other people inside its own
source tree. These are called **bundled copies**. Examples are the code that unpacks JPEG 2000
pictures and the code that reads ZIP files. A bundled copy changes only when someone here
replaces it by hand. A security fix made by the library's authors therefore never reaches the
Pi on its own.

Debian 13 (trixie), which the Raspberry Pi runs, also packages many of these libraries.
Debian's security team follows the bugs reported in them and ships the fixes through the usual
`sudo apt update && sudo apt upgrade`. Where Debian has a copy that works for SatDump, the Pi
build therefore uses **Debian's copy**. The bundled copy is kept only for the Windows, macOS and
Android builds, which have no such package system.

This file lists every bundled library with the following details:
- where it is and which version it is;
- where it came from and its licence;
- what SatDump changed in it;
- what Debian offers instead;
- whether the Pi build compiles it;
- what in SatDump uses it;
- whether data from a radio or from the network can reach it;
- known open security problems (CVEs).

The same list, in a form a program can read, is `tests/deps/third_party.json`. The test gate
(`tests/gate.sh`) runs `tests/deps/check_deps.py`, which fails when any of these changes and
the list was not updated with it:
- a bundled library's files;
- its version;
- a new folder or file in `src-core/libs`, or in one of the folders where bundled files sit
  next to SatDump's own (`src-core/imgui`, `src-ui/imgui`, `src-core/angelscript`,
  `src-core/image/font`, `src-core/image/font_lib` and
  `plugins/firstparty_support/fcidecomp`).

It also fails when a library in the list has no row in Table 1, or when that row does not
start its version cell with the version in the list. So the list and the version column cannot
drift apart silently. The other columns (Debian version, CVEs, what uses the library) are kept
up to date by hand, as step 5 of the review routine says.

### How to check for updates

1. **Debian's copies (on the Pi).** `apt list --upgradable` shows waiting updates.
   - Debian's security announcements are at <https://www.debian.org/security/>.
   - One library's open problems are at
     `https://security-tracker.debian.org/tracker/source-package/<name>`. Use the source
     package name, for example `openjpeg2`, `libaec`, `bzip2`, `muparser` or
     `libjpeg-turbo`.
2. **Bundled copies.**
   - Compare the version in Table 1 with the releases page at the upstream link.
   - Search for the library's name in the Debian security tracker and at
     <https://www.cve.org/>.
3. **The drift check.** `python3 -I -B tests/deps/check_deps.py .` run from the source folder
   prints `ok` when the tree matches the list.

The Rust code (`rust/`, off by default) has no third-party crates, so nothing in it needs
tracking here. Data files under `resources/` (maps, fonts, lookup tables) are not code and are
not listed.

## What this step changed (plan item M4)

After the M4 changes, the Pi (Linux) build behaves like this:

- **From Debian instead of the bundled copy.** The bundled copies stay in the tree for
  Windows, macOS and Android.

  | Library | Debian 13 build package | Runtime package |
  |---|---|---|
  | bzip2 | libbz2-dev 1.0.8-6 | libbz2-1.0 |
  | OpenJPEG | libopenjp2-7-dev 2.5.3-2.1~deb13u2 | libopenjp2-7 |
  | libaec | libaec-dev 1.1.3-1 | libaec0, libsz2 |
  | muparser | libmuparser-dev 2.3.4-1 | libmuparser2v5 |
  | 8-bit libjpeg | libjpeg62-turbo-dev 1:2.1.5-4 | libjpeg62-turbo |

  SatDump's own "read a JPEG 2000 picture from memory" function used to live inside the
  bundled OpenJPEG. It now lives in SatDump's own code.
- **Bundled but never compiled.**
  - muparserx: its 39 files were compiled, but nothing called them.
  - fkYAML: nothing includes it.
- **Still bundled and compiled, updated.** MQTT-C is now release 1.1.7 (it was a 2022
  snapshot after 1.1.6), which fixes CVE-2026-54412. Debian has no MQTT-C package.
- **Still bundled and compiled, unchanged.** Every other library in Table 2 marked
  "Yes".
- **Build cleanup.** Stale entries in the `src-core/CMakeLists.txt` file list are removed:
  `libs/libtom` and `libs/tiny-regex-c` do not exist, and `libs/bzlib` was listed twice.

**Still open:** libaec has CVE-2026-120457, an out-of-bounds read in `m_se_decode`.
- It affects every libaec up to 1.1.7: both the bundled copy and Debian 13's 1.1.3-1.
- The fix is in libaec 1.1.8, which Debian has only in its next release so far.
- With Debian's copy, the fix will arrive through `apt` once Debian ships it.
- SatDump reaches libaec only when decoding GOES, JPSS and Landsat data.

## Table 1: what each bundled library is

"Local changes" means changes SatDump made to the library's own files, as found in the git
history. Changes made by this fork are marked *(fork)*. Each version cell starts with the version
as `tests/deps/third_party.json` records it; "unversioned" means the library's files carry no
version number.

| Library | Path | Version, and where it is written | Upstream | Licence | Local changes |
|---|---|---|---|---|---|
| **Core libraries** | | | | | |
| libaec | `src-core/libs/aec` | 1.0.x (2016), no version string; `Copyright.txt` ends at 2016 | <https://gitlab.dkrz.de/k202009/libaec> | BSD-2-Clause | `gcc_builtins.c` removed and `config.h` adjusted (2022, GOES Rice fix) |
| base64 | `src-core/libs/base64` | unversioned (a code snippet) | <https://gist.github.com/tomykaira/f0fd86b6c73063283afe550bc5d77594> | MIT | none |
| CCSDS 122 Bit Plane Encoder | `src-core/libs/bpe` | 2006-11-03 (file headers) | University of Nebraska-Lincoln reference code by Hongqiang Wang; no maintained upstream | University of Nebraska-Lincoln licence agreement (its terms are not in the tree) | SatDump's wrapper (`bpe.c`, `bpe.h`); Windows fix in `errorhandle.c` |
| bzip2 | `src-core/libs/bzlib` | 1.1.0, a development snapshot (`bz_version.h`, `BZ_VERSION`) | <https://gitlab.com/bzip2/bzip2> | bzip2 licence (BSD-style) | the `bzip2` command-line program removed |
| CALCEPH | `src-core/libs/calceph` | 4.0.5 (`calceph.h`, `CALCEPH_VERSION_MAJOR/MINOR/PATCH`) | <https://www.imcce.fr/inpop/calceph> | CeCILL-C, CeCILL-B or CeCILL 2.1 | Windows fix in `calcephconfig.h` |
| libcorrect | `src-core/libs/correct` | unversioned | <https://github.com/quiet/libcorrect> | BSD-3-Clause | only the Reed-Solomon part kept |
| CTPL thread pool | `src-core/libs/ctpl` | unversioned (2014) | <https://github.com/vit-vit/CTPL> | Apache-2.0 | none |
| deepspace-turbo | `src-core/libs/deepspace-turbo` | unversioned (2017) | <https://github.com/geeanlooca/deepspace-turbo> | MIT | warning fixes |
| dlfcn-win32 | `src-core/libs/dlfcn` | unversioned | <https://github.com/dlfcn-win32/dlfcn-win32> | MIT | Windows fixes (2024) |
| fkYAML | `src-core/libs/fkyaml` | 0.4.3 (`node.hpp`, `FK_YAML_MAJOR/MINOR/PATCH_VERSION`) | <https://github.com/fktn-k/fkYAML> | MIT | none |
| IJG libjpeg, 8-bit | `src-core/libs/jpeg` | 6b (27-Mar-1998), in `jversion.h` (`JVERSION`) | <https://www.ijg.org/>; this copy is DCMTK's `libijg8` | IJG licence | symbols renamed so the 8- and 12-bit copies can coexist; Android tweaks in `jinclude.h` and `jdatasrc.c` |
| IJG libjpeg, 12-bit | `src-core/libs/jpeg12` | 6b (27-Mar-1998), in `jversion12.h` (`JVERSION`) | <https://github.com/DCMTK/dcmtk/tree/master/dcmjpeg/libijg12> | IJG licence | as DCMTK ships it |
| libintl-tiny | `src-core/libs/libintl-tiny` | unversioned | libintl-lite; maintained fork <https://github.com/j-jorge/libintl-lite> | BSL-1.0 | none |
| libOpenCL_loader | `src-core/libs/libopencl_loader` | unversioned (2019) | Guohui Wang's loader; `CL/` headers from <https://github.com/KhronosGroup/OpenCL-Headers> | loader: no licence stated; headers: Apache-2.0 | none |
| miniz | `src-core/libs/miniz` | 10.2.0, which is release 2.2.0 (`miniz.h`, `MZ_VERSION`) | <https://github.com/richgel999/miniz> | MIT | none |
| MQTT-C | `src-core/libs/mqttc` | 1.1.7 (tag v1.1.7, commit 102e59c1), in `README.md` (`Version:`); was a 2022 snapshot after 1.1.6 | <https://github.com/LiamBindle/MQTT-C> | MIT | local `"mqtt.h"` includes; Windows tweaks in `posix_sockets.h` |
| muparser | `src-core/libs/muparser` | 2.3.3 (Development Build), in `muParserDef.h` (`ParserVersion`) | <https://github.com/beltoforion/muparser> | BSD-2-Clause | Windows fix in `muParserFixes.h` |
| muparserx | `src-core/libs/muparserx` | 4.0.12 (2023-03-04), in `mpDefines.h` (`MUP_PARSER_VERSION`) | <https://github.com/beltoforion/muparserx> | BSD-2-Clause | none |
| OpenJPEG | `src-core/libs/openjp2` | 2.5.0 (`opj_config_private.h`, `OPJ_PACKAGE_VERSION`) | <https://github.com/uclouvain/openjpeg> | BSD-2-Clause | build fixes for Windows, macOS and ARM64; AVX turned off; SatDump's read-from-memory stream |
| NetBSD strptime | `src-core/libs/others` | 1.62 (2017/08/24), in `strptime.cpp` (`$NetBSD$` line) | <https://cvsweb.netbsd.org/bsdweb.cgi/src/lib/libc/time/strptime.c> | BSD-2-Clause | made into C++ for Windows |
| libpredict | `src-core/libs/predict` | 3.0.0 (`predict.h`, `PREDICT_VERSION_STRING`) | <https://github.com/la1k/libpredict> | GPL-2.0 | many: Windows fixes (2022, 2026), time fixes (2024), TLE phase-out in `orbit.c` (2026) |
| SuperNOVAS | `src-core/libs/supernovas` | 1.5.0-devel (`novas.h`, `SUPERNOVAS_*_VERSION` and `RELEASE_STRING`) | <https://github.com/Sigmyne/SuperNOVAS> | Unlicense | Windows fixes |
| CLI11 | `src-core/libs/CLI11.hpp` | 2.5.0 (`CLI11_VERSION`) | <https://github.com/CLIUtils/CLI11> | BSD-3-Clause | none |
| QOI | `src-core/libs/qoi.h` | unversioned | <https://github.com/phoboslab/qoi> | MIT | none |
| RapidXml | `src-core/libs/rapidxml.hpp` | 1.13 (`// Version 1.13`) | <https://rapidxml.sourceforge.net/> | BSL-1.0 or MIT | none |
| mmap for Windows | `src-core/libs/mmap_windows.h` | unversioned | Mike Frysinger's mmap() replacement | public domain | none |
| strings.h for Windows | `src-core/libs/strings_h_win.h` | unversioned | <https://github.com/win32ports/strings_h> | MIT | none |
| **User interface** | | | | | |
| Dear ImGui | `src-core/imgui/imgui.h` and the other `imgui*`, `imconfig.h` and `imstb_*` files there | 1.92.5 (`imgui.h`, `IMGUI_VERSION`) | <https://github.com/ocornut/imgui> | MIT | soft skin in `imconfig.h`, `imgui_draw.cpp` and `imgui_widgets.cpp` *(fork)*; warning fix in `imconfig.h` *(fork)*; earlier SatDump fixes (2023-2024). `imgui_soft.h`, `imgui_image.*`, `imgui_flags.h`, `imgui_filedrop.h` and `dialogs/` are SatDump's own |
| Dear ImGui backends | `src-ui/imgui/imgui_impl_glfw.cpp` and the `imgui_impl_opengl2/3` files | 1.92.5, updated with Dear ImGui (no version string) | <https://github.com/ocornut/imgui/tree/master/backends> | MIT | OpenGL 2 and OpenGL ES tweaks, mipmapped textures (2023) |
| gl3w | `src-ui/imgui/gl3w` | unversioned (generated) | <https://github.com/skaslev/gl3w> | Unlicense; Khronos headers MIT-style | extensions removed |
| portable-file-dialogs | `src-ui/imgui/portable-file-dialogs.h` | unversioned | <https://github.com/samhocevar/portable-file-dialogs> | WTFPL | none |
| ImPlot | `src-core/imgui/implot` | 0.18 WIP (`implot.h`, `IMPLOT_VERSION`) | <https://github.com/epezent/implot> | MIT | spelling fixes |
| ImPlot3D | `src-core/imgui/implot3d` | 0.1 (`implot3d.h`, `IMPLOT3D_VERSION`) | <https://github.com/brenocq/implot3d> | MIT | Windows and macOS math fixes |
| imnodes | `src-core/imgui/imnodes` | unversioned | <https://github.com/Nelarius/imnodes> | MIT | warning fix in `imnodes_internal.h` *(fork)* |
| imgui_markdown | `src-core/imgui/imgui_markdown.h` | unversioned | <https://github.com/enkisoftware/imgui_markdown> | Zlib | warning fixes |
| imgui-notify | `src-interface/imgui_notify` | unversioned | <https://github.com/patrickcjk/imgui-notify>; `font_awesome_5.h` from <https://github.com/juliettef/IconFontCppHeaders> | MIT; icon header Zlib | always-on-top and Windows fixes |
| nativefiledialog-extended | `src-ui/nfd` | 1.2.1 (`CMakeLists.txt`, `project(... VERSION)`) | <https://github.com/btzy/nativefiledialog-extended> | Zlib | defaults to the desktop portal (D-Bus); Windows fix |
| imgui_sw, a software renderer for Dear ImGui | `src-core/dsp/benchmark/render/imgui_sw.cpp` and `imgui_sw.hpp` | unversioned (2018) | <https://github.com/emilk/imgui_software_renderer> | public domain, or a do-as-you-like licence (dual, in the file header) | adapted to newer Dear ImGui |
| ImGuiColorTextEdit | `src-core/handlers/dataset/TextEditor.cpp` and `TextEditor.h` | unversioned | <https://github.com/BalazsJako/ImGuiColorTextEdit> | MIT | small changes for newer Dear ImGui |
| Dear ImGui Android backends | `android/imgui_backends` | about 1.90 (newest change note 2023-10-05; no version string) | <https://github.com/ocornut/imgui/tree/master/backends>; the OpenGL loader from <https://github.com/dearimgui/gl3w_stripped> | MIT; loader public domain | Android fixes: S-Pen (2024), right click with a long press (2026) |
| **Data formats and scripting** | | | | | |
| nlohmann json | `src-core/nlohmann/json.hpp` | 3.11.3 (`NLOHMANN_JSON_VERSION_MAJOR/MINOR/PATCH`) | <https://github.com/nlohmann/json> | MIT | small Windows and warning fixes; `json_utils.*` next to it is SatDump's own |
| AngelScript | `src-core/angelscript/lib` (and the copy `src-core/angelscript/angelscript.h`) | 2.38.0 WIP (`lib/angelscript.h`, `ANGELSCRIPT_VERSION_STRING`) | <https://www.angelcode.com/angelscript/> | Zlib | Android and macOS fixes |
| AngelScript add-ons | `src-core/angelscript/scriptarray`, `scriptbuilder`, `scriptdictionary`, `scriptstdstring` | 2.38.0 WIP, the same SDK as AngelScript (no version string) | <https://www.angelcode.com/angelscript/> | Zlib | Android fixes; `scriptsatdump/` is SatDump's own |
| **Plugins** | | | | | |
| CharLS | `plugins/firstparty_support/fcidecomp/charls` | 2.4.2 (`version.h`, `CHARLS_VERSION_MAJOR/MINOR/PATCH`) | <https://github.com/team-charls/charls> | BSD-3-Clause | none |
| EUMETSAT FCIDECOMP | `plugins/firstparty_support/fcidecomp/H5Zjpegls.c` and the other `H5Zjpegls*` and `fcicomp_*` files there | 2.x (same files as 2.1.1), no version string. The Apache-2.0 headers and CharLS 2 mean release 2.0.0 or later, and the files match Debian's 2.1.1 source apart from SatDump's changes. `VERSION:1.0.2` in `H5Zjpegls_definitions.h` is an old change note | <https://gitlab.eumetsat.int/sepdssme/fcidecomp> | Apache-2.0 | `#include "..."` instead of `<...>`; `register_MTG_FILTER()` registers the filter in place of the HDF5 plugin entry points; logging turned on in `fcicomp_log.h` |
| LibTomCrypt, DES part only | `plugins/gk2a_support/libtom` | 1.17 (`tomcrypt.h`, `SCRYPT`) | <https://github.com/libtom/libtomcrypt> | public domain (1.17 terms) | cut down to DES |
| libacars | `plugins/inmarsat_support/aero/libacars` | 2.1.x (no version string; the file holding `LA_VERSION` is not bundled) | <https://github.com/szpajder/libacars> | MIT | Windows patches |
| mbelib | `plugins/inmarsat_support/aero/mbelib` | 1.3.0 (`README.md`) | <https://github.com/szechyjs/mbelib> | ISC | Windows fix |
| librfnm | `plugins/sdr_sources/rfnm_sdr_support/librfnm` | unversioned (an older librfnm, chosen on purpose) | <https://github.com/rfnm/librfnm> | MPL-2.0 | Windows and macOS fixes; warning fix in `librfnm.h` *(fork)* |
| ExtIO_sddc / libsddc | `plugins/sdr_sources/sddc_sdr_support/lib` | 1.3.0 RC1 (`Core/config.h`, `SWVERSION`) | <https://github.com/ik1xpv/ExtIO_sddc> | MIT; bundled PFFFT BSD-style | none |
| libmirisdr-4 | `plugins/sdr_sources/mirisdr_sdr_support/mirisdr` | unversioned | <https://github.com/f4exb/libmirisdr-4> | GPL-2.0-or-later | none |
| SDRplay API headers | `plugins/sdr_sources/sdrplay_sdr_support/sdrplay` | 3.15 (`sdrplay_api.h`, `SDRPLAY_API_VERSION`) | <https://www.sdrplay.com/api/> | SDRplay API licence (headers only; the library is loaded at run time) | none |
| SDR++ server client | `plugins/sdr_sources/sdrpp_server_support/sdrpp_server` | unversioned | <https://github.com/AlexandreRouma/SDRPlusPlus> | GPL-3.0 | many SatDump changes; logger fix *(fork)* |
| SpyServer client | `plugins/sdr_sources/spyserver_support/spyserver` | unversioned | <https://github.com/AlexandreRouma/SDRPlusPlus> (from SDR++) | GPL-3.0; protocol header by Airspy | crash fix (2024), Windows fixes |
| serial (wjwwood) | `plugins/experimental_devices_support/libserial` | 0.1 (`serial.h`, `\version`; upstream never updated it) | <https://github.com/wjwwood/serial> | MIT | none |
| PublicDecompWT | `plugins/xrit_support/DecompWT` | unversioned | <https://gitlab.eumetsat.int/open-source/PublicDecompWT> | Apache-2.0 | HRIT decoding fix (2026) |
| SOHO Rice decompressor (`ricerecon64`) | `plugins/stereo_support/stereo/instruments/secchi/rice_decomp.cpp` and `rice_decomp.h` | 1996-07-11 (file header) | U.S. Naval Research Laboratory code for SOHO LASCO/EIT by Mitchell R. Grunes and Karl Hoppel; no public upstream known | not stated in the file | ported to C++ to work from memory and not leak on errors (2023); warning fixes |
| LDPC decoder | `plugins/dvb_support/codings/dvb-s2/ldpc` | unversioned (2018) | <https://github.com/xdsopl/LDPC> | 0BSD | small fixes |
| BCH decoder | `plugins/dvb_support/codings/dvb-s2/bch` | unversioned (2018) | GNU Radio gr-dtv, <https://github.com/gnuradio/gnuradio> | GPL-3.0-or-later | none |
| **Build helpers, image text and tests** | | | | | |
| ghc::filesystem | `std_filesystem/filesystem` | 10510, which is release 1.5.10 (`GHC_FILESYSTEM_VERSION`) | <https://github.com/gulrak/filesystem> | MIT | none |
| VOLK `volk_alloc.hh` | `volk_includes` | unversioned (2019) | <https://github.com/gnuradio/volk> | GPL-3.0-or-later | none |
| doctest | `tests/third_party/doctest` | 2.4.12 (`doctest.h`, `DOCTEST_VERSION_MAJOR/MINOR/PATCH`) | <https://github.com/doctest/doctest> | MIT | none |
| stb, copies for drawing text on images | `src-core/image/font/imstb_truetype.h`, the other `imstb_*` files there and in `src-core/image/font_lib` | 1.26 (stb_truetype); stb_rect_pack 1.01 and stb_textedit 1.14 (file headers) | <https://github.com/nothings/stb> | MIT or public domain | none |
| utfcpp | `src-core/image/font/utf8.h` and `src-core/image/font/utf8` | 2.x (no version string) | <https://github.com/nemtrif/utfcpp> | BSL-1.0 | none |
| libdc1394 Bayer decoder | `src-core/image/bayer` | unversioned (libdc1394 2.x); `bayer.cpp` differs from libdc1394 2.2.6's `dc1394/bayer.c` mostly in formatting | <https://sourceforge.net/projects/libdc1394/> | LGPL-2.1-or-later; its VNG and AHD parts come from DCRAW | builds on its own as C++ (types and error codes in `bayer.h`); fixes (2023-2024) |
| readerwriterqueue | `src-core/dsp/base/atomicops.h` and `src-core/dsp/base/readerwritercircularbuffer.h` | unversioned (newer than 1.0.6), because the circular buffer has `peek()` and `try_pop()`, which release 1.0.6 lacks | <https://github.com/cameron314/readerwriterqueue> | simplified BSD; the semaphore in `atomicops.h` Zlib | none recorded; the PowerPC cache-line size and the thread-sanitizer detection differ from upstream |
| maidenhead | `src-core/common/map/maidenhead.c` and `.h` | unversioned (2021) | <https://github.com/sp6q/maidenhead> | MIT | none |

## Table 2: where each library stands on the Pi

- **"Pi build"** says whether the Linux build compiles the bundled copy after this step.
- **"Radio or network data"** says whether bytes received from a radio or downloaded from
  the network reach the library. Those are the places where a bug could be triggered by
  someone else.
- **CVEs** were checked on 2026-11-11 in the Debian security tracker, and for libraries Debian
  does not package, in the published CVE lists. "None known" means no open entry was found.

| Library | Debian 13 equivalent | Pi build | Used for | Radio or network data | Known open CVEs (2026-11-11) |
|---|---|---|---|---|---|
| **Core libraries** | | | | | |
| libaec | libaec-dev 1.1.3-1 | No: Debian's copy | GOES HRIT/LRIT Rice, JPSS VIIRS and OMPS, Landsat | Radio | CVE-2026-120457: affects Debian's 1.1.3-1 and the bundled copy; fixed in 1.1.8, not yet in Debian 13 |
| base64 | none | Yes (header) | EUMETSAT product loader; the main window | Local files | None known |
| CCSDS 122 Bit Plane Encoder | none | Yes | Proba-V vegetation images | Radio | None known |
| bzip2 | libbz2-dev 1.0.8-6 | No: Debian's copy | MetOp and MetOp-SG admin messages, HimawariCast; AHI, AVHRR and MetOp file readers | Radio | Debian: only CVE-2026-42250, in the `bzip2recover` tool SatDump does not use. The bundled copy has the CVE-2019-12900 fix |
| CALCEPH | libcalceph-dev 4.0.4-2 | Yes | Planet and Moon positions, read from the bundled `de440s.bsp` file at start-up | No (bundled file) | None known |
| libcorrect | none | Yes | Reed-Solomon error correction on the CCSDS chains, including MetOp and Meteor | Radio | None known |
| CTPL thread pool | none (Debian's `ctpl` is a different library) | Yes (header) | Background work in live pipelines, the main window and GeoNetCast | No | None known |
| deepspace-turbo | none | Yes | CCSDS turbo decoder (deep-space missions) | Radio | None known |
| dlfcn-win32 | not needed | Compiled, but empty outside Windows | Loading plugins on Windows | No | None known |
| fkYAML | none | No: nothing includes it | nothing | No | None known |
| IJG libjpeg, 8-bit | libjpeg62-turbo-dev 1:2.1.5-4 | No: Debian's copy | JPEG pictures: xRIT (MSG, GK-2A, FY-4, Elektro), Proba SWAP, image loading, and map tiles downloaded for the GUI map | Radio and network (map tiles over HTTP) | Debian's libjpeg-turbo: none open. The bundled 1998 code gets no security maintenance |
| IJG libjpeg, 12-bit | none usable | Yes | 12-bit JPEG in GK-2A, FY-4, DSCOVR EPIC, MATS and Hinode | Radio | None tracked; 1998 code with no security maintenance |
| libintl-tiny | not needed (glibc has gettext) | Compiled, but empty on Linux | Translations on macOS, Windows and Android | No | None known |
| libOpenCL_loader | not needed | Compiled, but empty outside Android | OpenCL on Android | No | None known |
| miniz | none | Yes | ZIP and zlib: GOES LRIT, GK-2A (also a ZIP fetched over plain HTTP), ZIP file readers | Radio and network | None known (CVE-2018-12913 affected 2.0.7 only) |
| MQTT-C | none | Yes | Sending Elektro GGAK data and other messages to an MQTT broker | Network (broker replies) | None known; CVE-2026-54412 (topic length) fixed by the 1.1.7 update in this step |
| muparser | libmuparser-dev 2.3.4-1 | No: Debian's copy | Formulas in image composites (AVHRR and others), BitView and flowgraphs | No (formulas come from settings files and the user) | None known |
| muparserx | libmuparserx-dev 4.0.12-2 | No: nothing calls it | nothing | No | None known |
| OpenJPEG | libopenjp2-7-dev 2.5.3-2.1~deb13u2 | No: Debian's copy | JPEG 2000 pictures: GOES GRB, FY-4, GK-2A, ORB, and any file starting with the JPEG 2000 marker | Radio | Debian's 2.5.3: TEMP-1153269-522F63 (heap overflow in `opj_j2k_read_sod`) and CVE-2026-107080 (2.4.0 to 2.5.4, so the bundled 2.5.0 too) are open and not yet fixed in any Debian release; CVE-2023-39327, -39328, -39329 and CVE-2019-6988 are open but postponed (no security update planned). The bundled 2.5.0 also has CVE-2024-56826, CVE-2024-56827, CVE-2025-50952 and CVE-2026-6192, which Debian has fixed |
| NetBSD strptime | not needed | Compiled, but empty outside Windows | Reading dates on Windows | No | None known |
| libpredict | none | Yes | Satellite pass prediction and tracking from TLE orbit data | Network (TLE files downloaded over HTTP) | None known |
| SuperNOVAS | libsupernovas-dev 1.3.0-2 | Yes | Astronomy calculations; AWS navigation data | Radio (numbers only, not parsed text) | None known |
| CLI11 | libcli11-dev 2.4.1+ds-2 | Yes (header) | The `satdump` and `satdump-ui` command lines | No | None known |
| QOI | libqoi-dev 0+git20220615+ds-4 | Yes (header) | Reading and writing QOI image files | Local files | None known |
| RapidXml | librapidxml-dev 1.13-3 | Yes (header) | MetOp and MetOp-SG admin messages; AWS file loader | Radio | None known |
| mmap for Windows | not needed | No: Windows only | Windows | No | None known |
| strings.h for Windows | not needed | No: Windows only | Windows | No | None known |
| **User interface** | | | | | |
| Dear ImGui | libimgui-dev 1.90.1+ds-1 (too old) | Yes | The whole GUI | No | None known (Debian lists stb_truetype CVEs as "unimportant"; the copy here reads only SatDump's bundled fonts) |
| Dear ImGui backends | part of libimgui-dev (too old) | Yes | Drawing the GUI with GLFW and OpenGL | No | None known |
| gl3w | none | Yes | Loading OpenGL functions | No | None known |
| portable-file-dialogs | none | Included, but used only on macOS | File dialogs on macOS | No | None known |
| ImPlot | none | Yes | Plots in the GUI | No | None known |
| ImPlot3D | none | Yes | 3D plots in the GUI | No | None known |
| imnodes | none | Yes | Flowgraph editor | No | None known |
| imgui_markdown | none | Yes (header) | Help and credits text | No | None known |
| imgui-notify | none | Yes | Pop-up notifications | No | None known |
| nativefiledialog-extended | none | Yes (desktop portal backend) | File and folder dialogs on Linux | No | None known |
| imgui_sw, a software renderer for Dear ImGui | none | Yes (compiled into the core) | Drawing Elektro GGAK plots into pictures, without a screen | No (it draws plots SatDump makes) | None known |
| ImGuiColorTextEdit | none | Compiled into the core, but nothing uses it | nothing | No | None known |
| Dear ImGui Android backends | part of libimgui-dev (too old) | No: Android only | Drawing the GUI on Android | No | None known |
| **Data formats and scripting** | | | | | |
| nlohmann json | nlohmann-json3-dev 3.11.3-2.1 | Yes (header) | Settings, products, and data downloaded over HTTP (remote products and datasets, Horizons, EUMETSAT) | Network | None known |
| AngelScript | angelscript-dev 2.35.1+ds-3.1 (older) | Yes | User scripts | No (local scripts) | None known |
| AngelScript add-ons | part of angelscript-dev (older) | Yes | User scripts | No | None known |
| **Plugins** | | | | | |
| CharLS | libcharls-dev 2.4.2-2 | Yes | JPEG-LS in MTG FCI product files | Local files | None known |
| EUMETSAT FCIDECOMP | libfcidecomp-jpegls-dev 2.1.1-1 | Yes | MTG FCI product files | Local files | None known |
| LibTomCrypt, DES part only | libtomcrypt-dev 1.18.2+dfsg-7 | Yes | GK-2A LRIT decryption | Radio | None in the DES part (the fixed CVE-2019-17362 and CVE-2018-12437 are in code not bundled) |
| libacars | none | Yes | Inmarsat Aero ACARS messages | Radio | None known |
| mbelib | none | Yes | Inmarsat Aero voice | Radio | None known |
| librfnm | none | Yes | RFNM radios (USB) | USB device | None known |
| ExtIO_sddc / libsddc | none | No: the plugin is off by default | RX888 radios | USB device | None known |
| libmirisdr-4 | libmirisdr-dev 2.0.0-4 (the older original, not a drop-in) | Yes | MiriSDR radios (USB) | USB device | None known |
| SDRplay API headers | none (the vendor installs the library) | Yes (headers) | SDRplay radios | USB device, through the vendor library | None known |
| SDR++ server client | none | Yes | Receiving samples from an SDR++ server | Network | None known |
| SpyServer client | none | Yes | Receiving samples from a SpyServer | Network | None known |
| serial (wjwwood) | none | Yes (experimental devices plugin) | Serial-port devices | Serial port | None known |
| PublicDecompWT | none | Yes | Meteosat (MSG) HRIT wavelet pictures | Radio | None known |
| SOHO Rice decompressor (`ricerecon64`) | none | Yes | STEREO SECCHI pictures | Radio | None known |
| LDPC decoder | none | Yes | DVB-S2 decoding | Radio | None known |
| BCH decoder | none | Yes | DVB-S2 decoding | Radio | None known |
| **Build helpers, image text and tests** | | | | | |
| ghc::filesystem | none | No: only for compilers without `<filesystem>` | nothing on the Pi | No | None known |
| VOLK `volk_alloc.hh` | libvolk-dev 3.2.0-2 already has it | No: only for old VOLK | nothing on the Pi | No | None known |
| doctest | doctest-dev 2.4.11-1 | Tests only (not installed) | Unit tests | No | None known |
| stb, copies for drawing text on images | libstb-dev 0.0~git20241109.5c20573+ds-1 | Yes | Text on images, from SatDump's bundled fonts | No (bundled fonts) | CVE-2026-79515 and CVE-2026-18497 in stb_truetype, rated "unimportant" by Debian: they need a hostile font file |
| utfcpp | libutfcpp-dev 4.0.5-1 | Yes (header) | Text on images | No | None known |
| libdc1394 Bayer decoder | libdc1394-dev 2.2.6-5 (the whole camera library) | Yes | Colour pictures from the TUBIN satellite's cameras | Radio | None known |
| readerwriterqueue | libreaderwriterqueue-dev 1.0.6-1 | Yes (headers) | The lock-free buffer between DSP blocks (`readerwritercircularbuffer.h`; `atomicops.h` supplies its semaphore) | No | None known |
| maidenhead | none | Yes (nothing calls it at present) | Grid-square locators | No | None known |

## Borrowed code inside SatDump's own files

These pieces were copied or ported into SatDump's own source files and are maintained as SatDump
code. They are listed here for the licences, but the drift check does not hash them. "From" is
what the file's own comments say.

| Code | Where | From | Licence |
|---|---|---|---|
| Thin plate spline solver | `src-core/projection/thinplatespline.*` | GDAL warp API | MIT |
| Generic Viterbi kernel | `src-core/common/codings/viterbi/volk_k7_r2_generic_fixed.h` | GNU Radio VOLK | GPL-3.0-or-later |
| DVB-S2 transport stream parser | `src-core/common/codings/dvb-s2/bbframe_ts_parser.*` | Ron Economos, gr-dvbs2rx | GPL-3.0-or-later |
| DVB-S2 frame descrambler, BCH and LDPC wrappers | `plugins/dvb_support/codings/dvb-s2/bbframe_*` | Ahmet Inan and Ron Economos, gr-dtv | GPL-3.0-or-later |
| Hinode picture restoration | `plugins/hinode_support/hinode/instruments/common.h` | SolarSoft | BSD-2-Clause |
| CCSDS LDPC codes and encoder and decoder (`labrador`) | `src-core/common/codings/ldpc/labrador` | Adam Greig's labrador-ldpc (Rust), ported to C++ | MIT |
| CCSDS LDPC decoder | `src-core/common/codings/ldpc/ldpc_decoder*` | gr-ccsds, a GNU Radio module | GPL-3.0-or-later (as its build files state) |
| CCSDS turbo decoder wrapper | `src-core/common/codings/turbo/ccsds_turbo.*` | gr-ccsds-1 (khawatkom's copy of gr-ccsds) | GPL-3.0-or-later (as its build files state) |
| CCSDS derandomizer, Reed-Solomon wrapper, NRZ-M and NRZ-S decoders | `src-core/common/codings/randomization.*`, `reedsolomon/reedsolomon.*`, `differential/nrzm.*` and `nrzs.*` | Lucas Teske's SatHelper | MIT |
| LFSR, FIR filter design, random numbers, generic differential decoder | `src-core/common/codings/lfsr.h`, `src-core/common/dsp/filter/firdes.*`, `src-core/common/dsp/utils/random.*`, `src-core/common/codings/differential/generic.*` | GNU Radio; one noise function from the KC7WW / OH2BNS channel simulator | GPL-3.0-or-later |
| Decimation filter plans | `src-core/common/dsp/resamp/power_decim/plans.h` | SDR++ | GPL-3.0 |
| Vincenty distance and bearing formulas | `src-core/common/geodetic/vincentys_calculations.cpp` | Mike Gavaghan's Gavaghan.Geodesy, through airbreather's C# copy | free to use and change, no warranty (quoted in the file) |
| CMake search modules | `cmake/FindZSTD.cmake`, `cmake/FindOpenGLES2.cmake` | Facebook folly; freeminer | Apache-2.0; public domain |
| Soft theme colours | `tools/theme_gen/palettes.json` | T3 Code themes (see `tools/theme_gen/LICENSE-T3Code.txt`) | MIT |

## Debian packages the Pi needs

Versions are those in Debian 13 on 2026-11-11. The preset `cmake/fork/pi.cmake` turns on the GUI,
OpenMP and almost every plugin (`PLUGINS_ALL`; the remote SDR plugin stays off). OpenCL is on by
default.

### To build

| Package | Version | Needed for | Required? |
|---|---|---|---|
| build-essential, cmake, pkgconf | -, 3.31.6-2, 1.8.1-4 | compiling | Required |
| ninja-build | 1.12.1-1 | faster builds (`-G Ninja`) | Optional |
| libvolk-dev | 3.2.0-2 | signal processing kernels | Required |
| libfftw3-dev | 3.3.10-2 | FFTs (waterfalls, demodulators) | Required |
| libpng-dev | 1.6.48-1+deb13u6 | PNG pictures | Required |
| libtiff-dev | 4.7.0-3+deb13u3 | TIFF pictures | Required |
| libjemalloc-dev | 5.3.0-3 | memory allocator | Required |
| libnng-dev | 1.10.1-2 | network sample sources and sinks, network pipeline modules | Required |
| libzstd-dev | 1.5.7+dfsg-1 | compressed ZIQ recordings | Required |
| libcurl4-openssl-dev | 8.14.1-2+deb13u7 | downloads (orbit data, map tiles, products) | Required |
| libsqlite3-dev | 3.46.1-7+deb13u2 | SatDump's databases | Required |
| libbz2-dev | 1.0.8-6 | bzip2 (since this step) | Required |
| libopenjp2-7-dev | 2.5.3-2.1~deb13u2 | JPEG 2000 (since this step) | Required |
| libaec-dev | 1.1.3-1 | Rice decompression (since this step) | Required |
| libmuparser-dev | 2.3.4-1 | formulas (since this step) | Required |
| libjpeg62-turbo-dev | 1:2.1.5-4 | 8-bit JPEG (since this step) | Required |
| libglfw3-dev, libgl-dev | 3.4-3, 1.7.0-1 | the GUI window and OpenGL | Required for the GUI |
| libdbus-1-dev | 1.16.2-2 | GUI file dialogs through the desktop portal | Required for the GUI |
| libxrandr-dev and the X11 headers (libx11-dev, libxcursor-dev, libxext-dev, libxi-dev, libxinerama-dev) | trixie | GLFW's build files | Required for the GUI |
| libusb-1.0-0-dev | 2:1.0.28-1 | MiriSDR and RFNM radios | Optional |
| librtlsdr-dev | 2.0.2-2 | RTL-SDR radios | Optional |
| libairspy-dev, libairspyhf-dev | 1.0.10-3, 1.6.8-4 | Airspy radios | Optional |
| libhackrf-dev, libbladerf-dev, liblimesuite-dev, libuhd-dev, libiio-dev with libad9361-dev, libsoapysdr-dev | 2024.02.1-3, 0.2024.05-1, 23.11.0+dfsg-3, 4.8.0.0+ds1-2, 0.26-2 and 0.3-2, 0.8.1-5 | HackRF, bladeRF, LimeSDR, USRP, PlutoSDR and SoapySDR radios | Optional, one per radio type |
| libhdf5-dev | 1.14.5+repack-3 | HDF5 product files (first-party products, FCI) | Optional |
| libarmadillo-dev | 1:14.2.3+dfsg-1 | a faster, sturdier solver for thin plate spline projections (SatDump has its own fallback) | Optional |
| ocl-icd-opencl-dev | 2.3.3-1 | OpenCL acceleration | Optional |
| libomp-dev | 1:19.0-63 | OpenMP with clang (gcc brings its own libgomp) | Optional |
| portaudio19-dev | 19.6.0-1.2 | audio output plugin | Optional |
| librtaudio-dev | 6.0.1~ds-2+b1 | RtAudio audio output and sound-card sample source (`PLUGIN_RTAUDIO_SINK`, `PLUGIN_RTAUDIO_SDR_SUPPORT`, both off by default and in the Pi preset) | Optional |
| libboost-dev | 1.83.0.2+b2 | UHD's own headers need it for the USRP plugin; SatDump itself does not use Boost (`packages.builder` lists it) | Optional, with libuhd-dev |
| libavcodec-dev, libavformat-dev, libavutil-dev, libavfilter-dev | 7:7.1.5-0+deb13u1 | the SpaceX Falcon 9 video encoder (`USE_VIDEO_ENCODER`, off by default) | Optional |
| (not in Debian) libhydrasdr, libfobos, LiteX M2SDR (`libm2sdr`), LimeSuiteNG, libcpptrace | vendor builds | HydraSDR, Fobos, LiteX M2SDR and LimeSDR (new API) radios; cpptrace crash reports (libbacktrace is used without it). Each plugin is skipped when its library is not found | Optional |
| libbacktrace-dev | 0.1-4 | crash reports with a stack trace (`ENABLE_CRASHDUMP`) | Optional |
| python3 | trixie | the tests and lint checks (`SATDUMP_TESTS`) | Tests only |
| cargo, rustc, rust-clippy | trixie (Rust 1.85) | the Rust parts (`SATDUMP_WITH_RUST`, off by default) | Optional |

### To run (arm64 package names)

| Package | Needed for | Required? |
|---|---|---|
| libvolk3.2, libfftw3-single3, libpng16-16t64, libtiff6, libjemalloc2, libnng1, libzstd1, libcurl4t64, libsqlite3-0 | the core library | Required |
| libbz2-1.0, libopenjp2-7, libaec0, libsz2, libmuparser2v5, libjpeg62-turbo | the libraries this step takes from Debian | Required |
| libgomp1 | OpenMP | Required with the Pi preset |
| libglfw3, libgl1 / libopengl0, libdbus-1-3 | the GUI | Required for the GUI |
| xdg-desktop-portal with a backend such as xdg-desktop-portal-gtk | GUI file dialogs (Linux has not used zenity since 2025) | Recommended for the GUI |
| librtlsdr0, libairspy0, libairspyhf1, libusb-1.0-0 and the other radio libraries | the matching radio plugins | One per radio type |
| libhdf5-310, libhdf5-cpp-310, libhdf5-hl-310 | HDF5 product files | When built with HDF5 |
| libarmadillo14 | projections | When built with Armadillo |
| ocl-icd-libopencl1 plus a GPU OpenCL driver | OpenCL acceleration | Optional |
| libportaudio2 | audio output | Optional |
| librtaudio7 | RtAudio audio output and sound-card source | When built with them |
| libavcodec61, libavformat61, libavutil59, libavfilter10 | the Falcon 9 video encoder | When built with it |
| libbacktrace0 | crash reports | When built with it |

## Review routine

### On every change that touches a bundled library

1. Run `python3 -I -B tests/deps/check_deps.py .`. It names each library whose files or
   version changed.
2. Check that the change was meant: an update, a security fix, or a patch SatDump needs.
3. Run `python3 -I -B tests/deps/check_deps.py . --update`. It rewrites the hashes, and the
   versions it can read, and prints every change. Like the warning ratchet's `--update`, run it
   on purpose and never just to make the gate pass.
4. For a library without a version string, set its `version` in `tests/deps/third_party.json`
   by hand. The script reminds you.
5. Update Tables 1 and 2 above: version, local changes, Debian version, CVEs. The check makes
   sure the version cell of Table 1 starts with the version in the list; the rest is by hand.
6. A new library folder or file in a scanned folder (`src-core/libs` and the others under
   `"scan"` in the list) must be added to the list and to both tables. A file that is SatDump's
   own goes under `not_libraries` in the list. Third-party code copied anywhere else must be
   added by hand: as a library when whole files are copied, or to "Borrowed code" when it is
   ported into SatDump's own files.

### Monthly

1. For each Debian-provided library (bzip2, OpenJPEG, libaec, muparser, libjpeg-turbo), check
   its page in the Debian security tracker. On the Pi, run `apt list --upgradable`.
2. For the bundled libraries that radio or network data reaches, look for new releases and
   CVEs. Table 2 says which ones:
   - libcorrect, deepspace-turbo, the bit plane encoder, 12-bit libjpeg, miniz, MQTT-C, libpredict
     and RapidXml;
   - nlohmann json, LibTomCrypt, libacars and mbelib;
   - the SDR++ and SpyServer clients, PublicDecompWT, the SOHO Rice decompressor, the
     libdc1394 Bayer decoder, and the LDPC and BCH decoders.
3. Update the CVE column and the date at the top of this file.

### When merging upstream SatDump

1. Run the drift check after the merge. Every library upstream changed shows up by name.
2. For each one, read upstream's change and decide whether it is wanted.
3. Then run `--update` and refresh the tables as for any change.
4. Check that upstream did not bring back a bundled copy that the Pi build takes from Debian.
   A change to the file list in `src-core/CMakeLists.txt` is the usual sign.
