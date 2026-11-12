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
| CLI | `satdump legacy live`: a module that failed during the run is reported once everything has stopped, and the command exits with code 2 (it used to exit 0, or hang). An error once the radio and the pipelines run now stops the radio, the FFT and the pipelines before it is reported with exit code 2. Upstream, such an error left them running and hung for ever, past --timeout (`--http_server 8080` was one; it now ends the run at once with exit 1, see the `--http_server` rows). A pipeline that cannot start now stops the FFT blocks it started. In multi-VFO mode, a VFO that cannot start stops the VFOs and radio already started before the error is reported (SatDump used to crash or hang) | 2026-11-11 |
| Bug fix | Buffers between modules: stopping a reader or writer that is just about to wait now always wakes it. The wake-up could be lost, and stopping then hung | 2026-11-11 |
| Crash fix | GUI processing: a run that fails in the background is logged instead of closing SatDump. The busy flag is cleared so the controls come back. The offline processing window always reports that processing ended, also when the output folder cannot be made. Waiting for a previous run that never started is no longer a silently ignored error | 2026-11-11 |
| Crash fix | Recorder: errors from opening or starting a radio, or from starting processing, are caught whatever their type (only runtime_error was caught, and anything else closed SatDump). The error text is logged as text, so a '%' in it no longer breaks the log line. A samplerate that cannot be restored is now logged instead of ignored. An error while stopping is logged instead of lost, and the live pipeline is always released. The VFO list lock is let go on every way out | 2026-11-11 |
| Crash fix | Background task queue: a task that throws something other than a std::exception no longer closes SatDump, and failures are logged instead of silently dropped. A task pushed just as the queue's thread was ending is no longer left unrun | 2026-11-11 |
| Crash fix | Autotrack: a callback or event handler that throws at AOS, LOS or engage is logged and autotrack carries on. It used to close SatDump and leave its lock taken. Its lock is now let go on every way out (new TLEs, setting the station, engaging, choosing what to track). When no pass follows the last one, nothing is engaged on a pass that does not exist (this read past the end of the list). In multi mode each AOS/LOS event now carries its own pass instead of the first one in the list | 2026-11-11 |
| Crash fix | Scheduled tasks (Kepler, IERS and GOES DCP updates): a task that fails is logged and runs again at its next time. The error used to close SatDump, also during a pass | 2026-11-11 |
| Crash fix | `satdump legacy autotrack`: a live pipeline that cannot start at AOS is logged whatever the error, and its lock is always let go. An error other than runtime_error used to leave it taken, so the next pass waited for ever. A failure in the processing after a pass is logged instead of lost | 2026-11-11 |
| Crash fix | Background threads that save images (GOES GRB), read a baseband file, handle MQTT messages, or run a GUI button's task: an error is logged instead of closing SatDump. The image-saving queue is only read under its lock | 2026-11-11 |
| Crash fix | A log message that contains a % (a file name, "100%", an error text) is written exactly as it is; it used to be read as a printf format and could crash. Formatted log messages are no longer padded with zero bytes or cut at 1024 characters, a console/log-file line of 1024 bytes or more keeps its line break, and a format that cannot be printed logs its text instead of an empty line | 2026-11-11 |
| Tests | Unit tests for the logger (capture sink) and a compile check that the compiler reports wrong printf-style log calls and refuses log formats built at run time with arguments | 2026-11-11 |
| Crash fix | Text read at run time is no longer put into a log format: an AX.25 frame whose callsign holds a % (any received frame can) crashed the AX.25 decoder, even with trace output off; tile URLs, file paths, flowgraph node ids and VFO, source and block names had the same problem. Such text is now a %s argument, and a log format built at run time with arguments no longer compiles | 2026-11-11 |
| Bug fix | The compiler now checks every printf-style log call. 88 log lines that printed wrong or garbage numbers were fixed (20 of them in SDR plugins the Pi build leaves out): sizes, frequencies, samplerates and times printed with the wrong printf type, a missing % before the file size, {0} placeholders that never showed the SDR++ Server error code, and an error text printed with %d | 2026-11-11 |
| Bug fix | Saving a TIFF whose projection cannot be read now warns that the GeoTIFF tags were left out; a picture with no projection is saved without tags and no message, as before | 2026-11-11 |
| Bug fix | IERS entries that cannot be read are still skipped, now with one warning that counts them and names the first by date (MJD); rows with no values yet are skipped quietly (counted at debug level); a leap-second parsing error now shows its reason | 2026-11-11 |
| Bug fix | Doppler correction warns when the station position is 0/0 (none set in the settings or the pipeline parameters) and when a position value in the settings cannot be read; it used to correct for 0/0/0 without a word | 2026-11-11 |
| Bug fix | A small EUMETSAT download that is not an error report is still opened, now with a warning naming the file, its size and the reason | 2026-11-11 |
| Dependencies | Linux builds use Debian's bzip2 (libbz2-dev) instead of the copy in src-core/libs/bzlib. If it is missing, the configure step stops and names the package. Windows, macOS and Android still build the bundled copy | 2026-11-12 |
| Dependencies | Linux builds use Debian's OpenJPEG (libopenjp2-7-dev) instead of the bundled 2.5.0 copy. The memory stream for J2K data (GOES GRB, FY-4, GK-2A, ORB) is now SatDump's own and uses only OpenJPEG's public functions. OpenJPEG's error messages now go to the log. Saved .j2k files now carry OpenJPEG's 39-byte comment 'Created by OpenJPEG version 2.5.3', which the bundled copy left out; the pixels are the same | 2026-11-12 |
| Crash fix | Saving a 2-channel J2K picture crashed: the colour transform read a third channel. A picture with 5 or more channels was written past the end of a 4-entry array. Loading J2K data read past the decoded samples when a picture did not start at 0,0 or was subsampled, and shifted by a negative amount above 16 bits. J2K data in memory was read past its end for a negative length (a packet shorter than its header) and through a null pointer. These cases are now refused, with the reason logged | 2026-11-12 |
| Bug fix | J2K pictures under 16 pixels wide or high are now saved, with fewer resolution levels. Before, OpenJPEG refused them and nothing was written | 2026-11-12 |
| Tests | New fuzz harness fuzz_j2k for J2K decoding from memory and the conversion to a picture. It starts from raw input or from good codestreams that it cuts and patches, and uses a dictionary of codestream markers | 2026-11-12 |
| Dependencies | Linux builds use Debian's libaec (libaec-dev) for JPSS VIIRS/OMPS and GOES LRIT Rice decoding, instead of the bundled 1.0.x copy. Good and cut-off data decode byte for byte the same. Some corrupted packets are now refused, where the old copy read past the end of a table or of the input | 2026-11-12 |
| Build | On Windows, macOS and Android the bundled bzip2 and OpenJPEG headers now come before the include folders added for every target, and the bundled libaec headers come before a plugin's own include folders. Before, Homebrew's libaec.h and szlib.h (installed with hdf5 into /opt/homebrew/include) were used with the bundled libaec code. The bundled libaec folder is now on the include path only for targets that use it | 2026-11-12 |
| Packaging | The installed headers (include/satdump) no longer include muparserx, nor, on Linux, the bundled bzip2, OpenJPEG and libaec, whose functions libsatdump_core does not have. A plugin built against the installed headers now uses the system's, as SatDump does | 2026-11-12 |
| Build | muparserx is unused and is no longer compiled on any platform; its files stay in the tree. Three stale entries are gone from the core's source list: libs/libtom and libs/tiny-regex-c (no such folders) and a second libs/bzlib. On Linux the whole build drops from 1626 to 1558 compiled files, and satdump_core from 810 to 739 | 2026-11-12 |
| Packaging | The builder package list, the fork test workflow, the Linux jobs of the manual upstream build (all_build.yml) and the README's Linux build instructions (Debian, Red Hat, Alpine, Gentoo, openSUSE) now install bzip2, OpenJPEG and libaec development packages, which Linux builds need | 2026-11-12 |
| Dependencies | Linux builds use Debian's muparser (libmuparser-dev 2.3.4 to build, libmuparser2v5 to run) instead of compiling the bundled copy; Windows, macOS and Android keep the copy. Expressions may now be up to 19999 characters long (was 4999) | 2026-11-12 |
| Tests | Unit tests pin how the bundled 8-bit libjpeg decodes and encodes (hashes of gray/RGB, baseline/progressive, damaged streams, memory and file paths, and refusal of pictures over 65500 pixels wide), ready for a later switch to the system libjpeg | 2026-11-12 |
| Bug fix | Bundled 8-bit libjpeg: left shifts of negative numbers and bit-buffer shifts that overflowed (undefined behaviour, which stopped the sanitizer test build) now shift as unsigned, as libjpeg-turbo does; pictures and saved files are byte-for-byte the same | 2026-11-12 |
| Bug fix | JPEG reading and saving: when libjpeg refuses a picture (a damaged JPEG, or one over 65500 pixels wide) its memory is now released; before, each refused picture leaked about 20 kB (reading) to 45 kB (saving) | 2026-11-12 |
| Dependencies | MQTT-C, the MQTT client the GGAK modules use, updated from a 2022 snapshot to 1.1.7. A PUBLISH whose topic length runs past the packet is now refused, where it used to be read out of bounds (CVE-2026-54412); src-core/libs/mqttc/README.md records the version, the upstream commit and the local changes | 2026-11-12 |
| Tests | test_mqtt.cpp unpacks MQTT packets from byte buffers and feeds the receive loop through a local socket pair, with no network. Well-formed PUBLISH packets give the right topic and payload; a topic length past the packet, a truncated header and an over-long remaining length are refused; the other MQTT-C 1.1.7 fixes are tested too | 2026-11-12 |
| Build | MQTT-C's posix_sockets.h clears its address hints with memset instead of '= {0}', which removes its last compiler warning (baseline lowered by one) | 2026-11-12 |
| Bug fix | The MQTT client used by the GGAK modules logs an error once when its connection fails or the broker sends a bad packet; before, it stopped sending and receiving without a word | 2026-11-12 |
| Dependencies | DEPENDENCIES.md lists every bundled third-party library: where it is, version, origin, licence, SatDump's changes, Debian 13 equivalent, whether the Pi build compiles it, what uses it, whether radio or network data reaches it, and open CVEs. It also lists code borrowed into SatDump's own files, the Debian packages needed to build and run, and a review routine | 2026-11-12 |
| Tests | New gate check deps_check (tests/deps/check_deps.py) compares the bundled libraries with tests/deps/third_party.json (67 libraries, each with a sha256 of its files). It fails, with a plain message, when a library's files or version change, a listed path disappears, a new folder or file appears in src-core/libs or in a folder that mixes bundled and own files, or DEPENDENCIES.md does not show a library's listed version. `--update` rewrites the hashes and the versions it can read, and is run on purpose. deps_check_selftest proves this on a small fake tree | 2026-11-12 |
| Packaging | The Debian package is now called satdump-fork. It provides, conflicts with and replaces the upstream satdump package, so `sudo apt install ./satdump-fork_*.deb` on the Pi removes the old one. It has an 'unofficial fork' description, a maintainer and section hamradio. These settings apply on Linux only; the Windows NSIS settings are unchanged, and macOS no longer prints Debian settings when it configures | 2026-11-12 |
| Packaging | dpkg-shlibdeps now writes the package's Depends from what every program, library and plugin links against, searching SatDump's own lib and plugin folders. This replaces upstream's hand-written list, which mixed Debian releases, named libdbus-1-dev and left out libraries the program links to directly (armadillo, curl, hdf5-cpp, tiff, fftw3-single, volk 3.2, portaudio and others). Recommends is ca-certificates. Suggests is zenity, xdg-utils, mesa-opencl-icd and uhd-host; zenity alone would bring about 120 packages onto a headless Pi | 2026-11-12 |
| Dependencies | packages.builder lists dpkg-dev (it has dpkg-shlibdeps) and file (CPack runs it on every packaged file to find the programs and libraries for dpkg-shlibdeps). The Docker builder had neither | 2026-11-12 |
| Packaging | Making the Debian package now stops at once, before anything is installed, when dpkg-shlibdeps or file is missing or cannot be run. The message names the missing tool and says `sudo apt install dpkg-dev file` (cmake/fork/cpack_deb_tools.cmake, which CPack reads first). Before, CPack made a package with an empty Depends when dpkg-shlibdeps was missing, and stopped only after the whole install when file was missing. Other package types are not affected | 2026-11-12 |
| Packaging | The package version now sorts by release stage, then by commit date: 2.0.0~alpha+git<UTC date>.<time>.<hash>-1. Upstream's 2.0.0-alpha-<hash> was read by dpkg as revision <hash>, so a newer build could sort as older. A source tree without git gets 2.0.0~alpha-1, which sorts before every git build | 2026-11-12 |
| Build | scripts/build-deb.sh builds the satdump-fork .deb natively on arm64 Debian 13 (the Pi or an arm64 VM) in build-deb. It uses the Pi preset and -DCMAKE_INSTALL_PREFIX=/usr, uses ccache when installed, reconfigures each time so the version is current, and prints the .deb path. Before building it checks that dpkg-shlibdeps, file and every package in packages.builder are installed, so a radio whose -dev package is missing is not silently left out. SATDUMP_DEB_ALLOW_MISSING=1 builds anyway and lists the radios with no plugin. On a station it uses tests/guard.sh: it will not start during a pass or with less than 9 GB free (5 GB for the build plus the 4 GB kept for a recording), and it runs at low priority. Its guard messages start with build-deb. Install on the Pi with `sudo apt install ./satdump-fork_*.deb` | 2026-11-12 |
| Tests | tests/package/check_deb.sh <satdump-fork_*.deb> <baseline prefix> <recording> checks a built package without installing it. It checks dpkg-deb --info and --contents, unpacks the package, and runs ldd with and without LD_LIBRARY_PATH (no 'not found'). It checks every directly linked system library is in a package Depends names, and runs apt-get install --simulate as a normal user, alone and with packages.runner. It then runs the unpacked satdump on a MetOp AHRPT recording and compares every product with a baseline install, byte for byte. It is not part of tests/gate.sh, because the package is a separate full build; run it after scripts/build-deb.sh | 2026-11-12 |
| Tests | New gate test deb_settings (label cli, as it also reads the gate's install). It checks the build's CPackConfig.cmake without making a package: the name satdump-fork; generated Depends with no hand-written libraries or -dev packages; the private library and plugin folders dpkg-shlibdeps searches; Provides, Conflicts and Replaces satdump; prefix /usr; and a valid version that sorts by stage then commit date (7 fixtures). It also checks that making a .deb stops at once without dpkg-shlibdeps or file (5 cases), and that the Dockerfile copies the package by its name. It uses the cmake that configured the build, and is registered on Linux when dpkg is available | 2026-11-12 |
| Packaging | The Dockerfile now copies and installs satdump-fork_*.deb. Its builder stage gets dpkg-dev and file from packages.builder, which making the package with generated Depends needs. Those Depends come from dpkg-shlibdeps. packages.runner, installed first, still brings the extra tools such as zenity and uhd-host. packages.runner is unchanged: all its entries resolve on Debian 13 | 2026-11-12 |
| Crash fix | JPEG reading (8-bit and 12-bit): an error from libjpeg now always stops the decode. With ignore_errors (MSG and Elektro xRIT) the rows decoded before the error are kept and the rest are black; an error in the headers, or any error without ignore_errors, gives no picture. libjpeg's warnings are printed as before. Before, with ignore_errors libjpeg carried on past its own errors and could write past its buffers or crash | 2026-11-12 |
| Crash fix | JPEG reading (8-bit and 12-bit): a colour JPEG no longer writes past the end of the picture. A YCbCr one gives its brightness (first component); RGB and CMYK ones give no picture | 2026-11-12 |
| Bug fix | 12-bit JPEG reading now releases libjpeg's memory after every error; before, each refused 12-bit picture left it behind | 2026-11-12 |
| Bug fix | JPEG reading refuses a picture over 100 million pixels before setting memory aside (a damaged header could ask for up to 4.3 billion), and treats a negative length as no data instead of reading past the end | 2026-11-12 |
| Bug fix | Bundled 12-bit libjpeg: left shifts of negative numbers and bit-buffer shifts that overflowed (undefined behaviour) now shift as unsigned, as in the 8-bit copy; 12-bit pictures and streams are the same bit for bit | 2026-11-12 |
| Bug fix | Bundled 8-bit libjpeg: the progressive and lossless encoders' bit-buffer shift that overflowed (undefined behaviour) now shifts as unsigned, as the other shifts already do; saved files are byte-for-byte the same | 2026-11-12 |
| Crash fix | Bundled libjpeg (8-bit and 12-bit): a damaged Huffman table with a DC symbol over 15 (16 for lossless) no longer reads past the end of a table; the symbol is a bad code, as an impossible code already was (a warning, and 0) | 2026-11-12 |
| Crash fix | Bundled libjpeg (8-bit and 12-bit): a Huffman table number over 3, in a scan header or a table definition, is now refused before it is used, with libjpeg's usual error; before, the list of 4 tables was read past its end first | 2026-11-12 |
| Bug fix | Bundled libjpeg (8-bit and 12-bit): a lossless JPEG whose precision is not 2 to 16 bits, or whose point transform is not below its precision, is now refused; before, the decoder shifted by a negative amount or by more than an int holds | 2026-11-12 |
| Bug fix | MSG and Elektro-L xRIT: a JPEG segment libjpeg gives no picture for is now a warning with libjpeg's reason and a warning that the segment was not decoded; before, the reason was not logged and the segment was reported as "JPEG Depth should be 8". Strict JPEG callers (GK-2A, which then tries JPEG 2000) still get only a debug line | 2026-11-12 |
| Bug fix | JPEG reading code: `const` means const again after the bundled libjpeg headers, whose old configuration defined it as nothing for all the C++ that followed; the picture allocation's catch now takes `const std::exception &`, as the error rules ask | 2026-11-12 |
| Tests | Unit tests for 8-bit and 12-bit JPEG reading: the same pixels as before on good streams, pinned results for streams cut off at every byte, colour streams, oversized pictures, bad Huffman tables and lossless parameters, what is logged when a stream gives no picture, and no libjpeg memory left behind | 2026-11-12 |
| Tests | Fuzz harnesses for 8-bit and 12-bit JPEG reading (fuzz_jpeg, fuzz_jpeg12): random and damaged streams, starting from good ones made by the unit tests' encoder, in the clang fuzz build and its short ctest run | 2026-11-12 |
| Docs | DEPENDENCIES.md: the 8-bit libjpeg stays bundled on Linux too, because Debian 13's libjpeg-turbo 2.1.5 cannot read lossless JPEG and reads damaged streams differently (revisit at libjpeg-turbo 3.x); libjpeg62-turbo is dropped from the Pi package lists there, both libjpeg copies' fork changes are listed, and their user lists are corrected (FY-4 uses neither; the cubesat decoders use the 8-bit one through load_jpeg, which does not yet have decompress_jpeg's checks) | 2026-11-12 |
| Packaging | Every Linux package list now includes muparser, which Linux builds need since they use the system's copy: the three Linux jobs of the manual upstream build (all_build.yml), the Forgejo Debian build (which also gets bzip2, OpenJPEG and libaec), and the README's install lists for Debian, Red Hat, Alpine, Gentoo and openSUSE | 2026-11-12 |
| Tests | Differential test framework (`tests/diff`): each Rust part is run against the C++ that SatDump ships on edge, seeded (`SD_DIFF_SEED`, `SD_DIFF_CASES`), corpus and mutated cases, fed whole or in chunks; an exception or a Rust panic counts as a difference, and each difference is saved as a reproducer in `<build>/diff-failures` with its replay commands | 2026-11-12 |
| Tests | JSONL oracles: the C++ writes what each twin should give; small ones are committed in `tests/diff/oracles` and `cargo test` replays them in Rust without the C++; every oracle file needs a minimum number of cases | 2026-11-12 |
| Tests | Float rule for C++/Rust comparisons: Rust must match the shipped C++; the C++ being ported is built with `-ffp-contract=off` in every build (goldens checked), `std::fma` becomes `mul_add`, and a ULP tolerance is allowed only for float outputs no flag makes exact, never for integers or pixels | 2026-11-12 |
| Tests | The derandomiser comparison (bytes, soft symbols, bits; every length 0-4096, odd bit values, negative lengths, unaligned buffers, the golden CADUs) now runs on the framework, with permanent self-tests that a planted bug is caught | 2026-11-12 |
| Rust | Rust soft-symbol and one-bit-per-byte CCSDS derandomisers next to the byte one; only compared with the C++ so far, SatDump does not call them yet | 2026-11-12 |
| Rust | `satdump::rust::CppOnly`: a test guard that pauses Rust so the C++ code does the work; the differential tests run each comparison's C++ side under it | 2026-11-12 |
| Rust | Second Rust part: the CCSDS AOS packet splitter (`rust/src/aos`). In a build with Rust on, every AOS decoder gets its packets from it, exactly the C++ splitter's packets. If it fails, a new C++ splitter takes over from that frame (the packet in progress is lost, as after a lost frame), the failure is logged once and Rust is off for the rest of the run. Packet zones under 6 or over 65536 bytes, and insert zones outside 0 to 65536 bytes, stay on the C++ code | 2026-11-12 |
| Rust | `ccsds_aos::Demuxer` keeps its state out of its header (movable, not copyable): callers compile unchanged, but the core library and the plugins must be installed together. It also gets a strict mode (last constructor argument, off by default, the same in C++ and Rust; no decoder uses it yet), and drops one member it never used | 2026-11-12 |
| Tests | Differential twin `aos_demux`: the C++ and Rust splitters compared frame by frame on all 12 layouts SatDump uses and 8 more, strict mode on and off, with tiny, exact-fit, very long and largest packets, headers cut at every place, idle packets, odd first-header pointers, counter gaps, wraps and repeats, dropped, repeated and cut frames, and the golden MetOp and Meteor frames; a 16-case oracle is committed. `satdump-diff run` prints the cases and frames of each group a twin sorts its cases into | 2026-11-12 |
| Tests | `unit_aos_rust` (Rust against C++ in 6 layouts with damaged streams, a moved splitter, settings Rust does not take, and a Rust failure forced in the middle of a stream), C++ strict-mode unit tests and Rust unit tests; in a fuzz build with Rust, `fuzz_aos_demux` stops on any C++/Rust difference (bit 3 of its first byte turns strict mode on) | 2026-11-12 |
| Rust | Third Rust part: the Meteor LRPT MSU-MR segment decoder (`rust/src/lrpt`: header and time stamp, Huffman, quantisation, integer IDCT, 8 x 112 pixels), called by the Meteor plugin's `Segment` through `rust/bridge/rust_lrpt.h` when Rust is on; about 2 times faster. A bad argument leaves the packet to the C++; a Rust failure gives the C++ result for that packet, is logged once and switches Rust off for the rest of the run. Assembling the picture is still C++ | 2026-11-12 |
| Rust | In Rust builds `satdump_core` passes `SATDUMP_WITH_RUST` to the code that links it, so plugins can call a bridge; Rust-off builds keep their compile flags. The LRPT decoder needs no `-ffp-contract` flag: its one float step, the quantisation table, gives the same integers fused or not for all 256 quality factors | 2026-11-12 |
| Tests | Differential twins `lrpt_segment` and `lrpt_qtable`: header fields, flags, time stamp and all 896 pixels, on edge lengths, empty and header-only payloads, every quality factor and MCU number, encoded packets, every truncation and bit flip of good packets, seeded cases and the golden Meteor packets; small oracles committed. A twin can read corpus files itself (`corpus_reader`), so a fuzz crash file can be replayed with `satdump-diff` | 2026-11-12 |
| Tests | `diff_lrpt_fallback` (a Rust panic part-way through a packet gives the C++ segment, one log line, and Rust off); `cli_rust` checks that the installed Meteor plugin calls the Rust decoder; in a fuzz build with Rust, `fuzz_msumr_lrpt` stops on any C++/Rust difference | 2026-11-12 |
| Tests | Float rule corrected: a ported C++ file that needs `-ffp-contract=off` to match Rust gets it in Rust-on builds only, so Rust-off builds such as the Pi's keep their flags | 2026-11-12 |
| Settings | SatDump downloads nothing by default: no element sources or satellite numbers to fetch, element updates "Never", IERS updates off. The IERS addresses are settings (`iers_bulletin_url`, `leap_seconds_url`); the installed defaults are the https addresses SatDump always used, fetched only when the IERS update is switched on | 2026-11-12 |
| Settings | Offline data: `file://` addresses for the orbit elements (TLE text or OMM CSV) and the two IERS files are read once at each start, before anything is processed. A missing or damaged file keeps the data already in the database, with one warning | 2026-11-12 |
| Bug fix | Pass prediction takes the station height in metres; it was converted as if it were an angle, so a station at 2000 m was computed at 35 m | 2026-11-12 |
| Bug fix | A TLE built from OMM data writes the international designator in TLE form for pieces of one to three letters ("2019-074AB" as "19074AB"; only one-letter pieces were converted, and longer ones were cut to "2019-074"), padded with spaces as TLE text is, not with NUL characters | 2026-11-12 |
| Bug fix | Element files: a stray line 2, or an OMM header where a name goes, makes that entry unreadable instead of being taken as a satellite name | 2026-11-12 |
| Bug fix | IERS: JSON that is not a bulletin is refused before the database transaction starts, so none is left open; the IERS tab-separated leap-second list is read as well as the space-separated one | 2026-11-12 |
| CLI | `legacy live`, `record` and `autotrack` ask only the `--source` driver for radios; before, every driver was asked, including those that search the network | 2026-11-12 |
| CLI | An `--http_server` that cannot start (port taken, not `host:port`, a number, or no value) ends `legacy live` or `record` at once with exit 1 | 2026-11-12 |
| CLI | `legacy live` catches SIGINT and SIGTERM from its start, including while the radio opens, and then stops with exit 0 | 2026-11-12 |
| Crash fix | A live module is stopped once more after its task ends: a stop right after start-up could leave DSP blocks it started late running while they were destroyed (exit 134) | 2026-11-12 |
| Tests | `cli_offline`: a whole station run (settings, element and IERS files, a live run, products) inside a network namespace with no network, so any connection attempt fails the test; skipped (77) where namespaces are not allowed, a failure instead with `SATDUMP_REQUIRE_NETNS=1` | 2026-11-12 |
| Tests | `cli_supervision` (start, watch through `/api`, stop with SIGTERM, exit codes), and unit tests for pass heights, the offline data files and the single-driver radio probe | 2026-11-12 |
| Crash fix | `-v` / `--verbose`: the flag's value was a local of the command handler's constructor, so reading the options wrote into a stack frame that had already returned (found by the sanitizer build); it now lives in the handler | 2026-11-12 |
| Bug fix | DSP blocks, legacy and new: start() and stop() of one block now run one at a time. A SIGTERM while a live run was starting could hang SatDump for ever (a legacy block's thread was started again and then waited for), or leave a new-DSP block running after its stop() returned | 2026-11-12 |
| CLI | A legacy command (`live`, `record`, or a pipeline run through `legacy`) during which a DSP block failed now ends with exit 2, as the exit-code table says (it ended with 0) | 2026-11-12 |
| CLI | `--http_server` given as a number (`8080`) or with no value now ends `legacy live` and `record` at once with exit 1 and "Cannot start the web server on <value>: ..." (it threw and ended with exit 2). Every web-server error line now begins "Cannot start the web server on <value>:" | 2026-11-12 |
| Bug fix | Multi-VFO `legacy live`: every way out of the run (a VFO or the web server that cannot start, any error) now stops each VFO's frequency shifter. Only the normal end did; on the other ways out they were destroyed while running | 2026-11-12 |
| Tests | `cli_supervision`: a stop must end within 10 s of the SIGTERM, and `/api` must stop answering while the program is still seen running (the old check could pass when the port closed only at exit). `legacy record` is now checked for the single-driver probe and for a web server that cannot start | 2026-11-12 |
| Tests | `cli_thread_faults`: the fault test module waits until its failing DSP block has failed, so `legacy-block-fails` can no longer end with exit 0 because the block's thread started late. Three more runs of it on one CPU with SCHED_BATCH (`taskset`, `chrt`; Linux) check this; without the wait, 19 of 20 such runs ended with 0 | 2026-11-12 |
| Crash fix | An orbit element file with a value a TLE cannot hold is refused with one warning and changes nothing. Such values are a drag term (BSTAR) or second derivative of 1 or more, a value that is not a number, a date outside 2000-2099, or a catalogue number too big. Such a file used to be stored, and every command then stopped at start-up (exit 134). A tiny drag term no longer makes SatDump hang | 2026-11-12 |
| Crash fix | Orbit elements already in the database that cannot be written as a TLE are skipped with a warning; before, start-up dropped the whole database for the run, or stopped. An error while reading the element or IERS files at start-up is logged and never stops SatDump | 2026-11-12 |
| Bug fix | A TLE international designator that fills all 8 columns ("99025BGQ") is read as it is; the copy used to read past its end (the fix is partly in libpredict's `orbit.c`) | 2026-11-12 |
| Bug fix | An international designator with a quote in it is stored; it used to break the database insert for that satellite | 2026-11-12 |
| Bug fix | Leap-second list: refused with one warning, keeping the leap seconds already loaded, unless its times and TAI-UTC values both go up from line to line, it has its expiry line (`#@` and a time), and it matches its hash line (`#h`, the SHA-1 of its data that both published layouts end with). A list cut short inside its last line was stored as 3 s from 2017 on, and one cut short between two lines gave 36 s, not 37 | 2026-11-12 |
| Bug fix | Orbit element downloads (`http`) skip the entries they cannot read, values a TLE cannot hold among them, with one warning naming the address and the count; the other entries are used. Before, such entries were dropped without a word, and those satellites kept their old elements. A download tried again after failing part way no longer has the failed try's text in front of it | 2026-11-12 |
| Tests | `cli_offline` runs the installed defaults with a new database, so an update they scheduled by mistake would be due at once and show; new runs with an element file and a stored row that cannot be written as a TLE. It also fails when the shipped leap-second list is refused. Some unit tests download from a small HTTP server they run on 127.0.0.1 | 2026-11-12 |
| Tests | `tests/gate.sh` empties the check install folder before installing when it is inside the build folder (the default `<build>/check-prefix`). Installing over earlier runs had kept a core library from an install with another libdir in `<prefix>/lib`, which the cli tests load first, so they ran old code and passed. The new `cli_install_fresh` fails when the folder holds a file the last install did not write | 2026-11-12 |
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
| 2 | The command ran and failed (the log line `Error running command` says why), including when a module or DSP block failed during the run, so the output is incomplete |
| 3 | Radio problem: the radio is not connected, its driver is missing, or it could not be opened, set up or started (`legacy record` and `legacy live`) |

A crash ends the way the system reports crashes: by its signal (a shell shows 128 + the
signal number, for example 139 for a segmentation fault or 134 for an abort), so a service
manager can tell a crash from an ordinary failure.

## Error handling rules

Every fix in this fork follows these rules, and new code should too:

1. No error may leave a thread. Every thread body and background task ends with
   `catch (const std::exception &)` and `catch (...)`, logs the error with what failed (the
   pipeline, module or block), and records that it failed. `tests/lint/thread_catches.py`
   checks the thread entry points it lists.
2. Catch `const std::exception &`, never only `std::runtime_error`. `catch (...)` belongs at
   thread and process boundaries only.
3. Nothing is swallowed silently on decode, file or settings paths: at least a warning with
   the reason and the value used instead.
4. Destructors, `stop()` and signal handlers never throw, and `stop()` always stops everything
   it started. (`-Werror=terminate` turns a throw that can only end in `std::terminate` into a
   build error.)
5. In a live pass, a module that fails is logged, marked in the live stats with an `"error"`
   field, and its input is let through, so the recording and the other modules keep going. The
   pass ends at its normal stop or timeout, and the command then exits with code 2.
6. Exit codes are the ones in the table above; a crash ends by its signal.
7. A damaged settings file or database never stops SatDump: it logs a warning and uses
   the defaults (or an in-memory database), leaving the file as it is. Stored rows that cannot
   be used are skipped with a warning, and an error while reading the element or IERS files at
   start-up is logged; neither stops SatDump.
8. A lifetime bug, such as a DSP block destroyed while it still runs, stops SatDump at once
   with a log line naming it, rather than crashing later somewhere unrelated.
9. Log calls: a message given alone is written as it is, `%` included. Text read at run time is
   passed as a `%s` argument, never used as the format; the compiler checks the formats
   (`tests/compile/logger_format.sh`).

## Offline and downloads

SatDump downloads nothing by default: `kepler_update_interval` is "Never", `iers_auto_update`
is off, and `kepler_settings.urls_to_fetch` and `norads_to_fetch` are empty.

The IERS addresses are settings (`iers_bulletin_url`, `leap_seconds_url`). The installed
`satdump_cfg.json` holds the https addresses SatDump always used, and `settings.json` is merged
over it. An address is used when `settings.json` gives it as `{"value": "<text>"}`, or leaves the
setting out (or gives an object without `"value"`), which means the installed https address;
either is fetched only when `iers_auto_update` is on. Anything else is no address, gives one
warning, and nothing is fetched for it: `{"value": ""}`, `{"value": null}`, a value that is not
text, `null`, or a setting that is not an object. An object in place of the text, as in
`{"value": {...}}`, makes the whole `settings.json` unreadable, so the installed defaults are
used (rule 7). A station program should therefore always set both addresses itself.

A station program that fetches the data itself gives SatDump `file://` addresses:

```json
{
  "satdump_general": {
    "kepler_update_interval": { "value": "4 hours" },
    "iers_auto_update": { "value": true },
    "iers_bulletin_url": { "value": "file://<station dir>/data/finals2000A.daily.json" },
    "leap_seconds_url": { "value": "file://<station dir>/data/leap-seconds.list" }
  },
  "kepler_settings": {
    "urls_to_fetch": ["file://<station dir>/data/elements.tle"],
    "norads_to_fetch": []
  }
}
```

The files are then read once at each start, before anything is processed, but only when:

- for elements: the interval is not "Never", every source is `file://` and there are no
  satellite numbers to fetch;
- for IERS: the update is on and both addresses are `file://`.

With any `http` source, upstream's scheduled update on a background task applies, which can
come late or overlap with processing. A download is not all or nothing: entries it cannot read,
values a TLE cannot hold among them, are skipped with one warning naming the address and the
count, and the others are stored.

What each file changes:

| File | Rule |
|---|---|
| Orbit elements (TLE text, with or without name lines, or OMM CSV) | All or nothing. A missing, empty or damaged file changes nothing, with one warning. Damaged means a bad checksum, line numbers that differ, a stray line 2, a header where a name goes, or a value a TLE cannot hold (see the changes table). A good file updates each satellite it lists. Elements already stored that cannot be written as a TLE are skipped, with a warning |
| Leap-second list (IERS tab or space layout) | All or nothing, in the same way. The list is also refused unless its times and its TAI-UTC values both go up from each entry to the next, it has its expiry line (`#@` and a time), and it matches its hash line (`#h`, the SHA-1 of the digits of the `#$` and `#@` times and of the entries). Both published layouts have these lines; comments, spaces and line ends may differ, but a list cut short or with a value changed is refused. Give SatDump the published file as it is (from IERS, or a copy such as the one in tzdata) |
| IERS bulletin (`finals2000A.daily.json`) | JSON that is not a bulletin changes nothing. Otherwise each readable entry is stored by date, and unreadable entries are skipped with one warning that counts them |

The database (`main.db`) keeps the last good data, so a station with no new files keeps working.
Write each file atomically (a temporary file, then a rename) before starting SatDump.

Radio discovery can use the network too (UHD broadcasts, PlutoSDR scans `usb:` and `ip:`,
SoapySDR lists remote devices). `legacy live`, `record` and `autotrack` ask only the driver
of their `--source`; `sdr_probe` and the GUI still ask every driver.

## Running under a station program

A program that starts, watches and stops SatDump for each pass can rely on the following.

Folder and settings: give every run `HOME=<station dir>/home` and
`XDG_CONFIG_HOME=<station dir>/home/.config`. SatDump then keeps `settings.json` and `main.db`
in `<station dir>/home/.config/satdump`. The settings file is merged over the installed
defaults, and arrays replace the defaults rather than add to them. Run SatDump with the pass
folder as its working directory: a crash writes `satdump_crash_<time>.txt` there.

Live run:

```
satdump legacy live metop_ahrpt <pass>/live-N --source airspy [--source_id <decimal serial>] \
  --samplerate 6000000 --frequency 1701300000 --gain_type 0 --general_gain <0-21> --bias true \
  --http_server 127.0.0.1:<port> [--timeout <s>]
```

- Airspy reads only `gain_type` (0 sensitive, 1 linear, 2 manual), `general_gain`,
  `lna_gain`, `mixer_gain`, `vga_gain`, `bias`, `lna_agc` and `mixer_agc`. Its defaults are
  gain 0 with the bias tee off. `--source_id` is the decimal serial (`sdr_probe` shows it in
  hex).
- RTL-SDR (`--source rtlsdr`) reads `gain`, `agc`, `lna_agc`, `tuner_agc`, `bias` and
  `ppm_correction`.
- A value follows its flag after a space. `true` and `false` become booleans and numbers are
  parsed; `source_id` stays text. A bare flag is true only as the last argument or before
  another `--flag`. Unknown or misspelt flags are ignored silently.
- Live writes only `live-N/<pipeline>.cadu` (whole 1024-byte frames). `dataset.json` appears
  only with `--finish_processing`, which a station program should not use.

Watching: `GET http://127.0.0.1:<port>/api` returns each module's state as JSON (for example
`deframer_state` is `SYNCED` when the decoder is locked). It does not answer during start-up
(from about 0.1 s to a few seconds, longer while a real radio opens), so poll with retries. If
the port is taken, or the value is not `host:port` (including a number such as `8080`, or the
flag given no value), the run exits 1 at once and logs a line that begins
"Cannot start the web server on <value>:" and gives the reason.

Stopping: SIGTERM or SIGINT is caught from the start of `legacy live`, including while the
radio opens; the run then exits 0 once start-up has finished, and the `.cadu` keeps its whole
frames. Measured stops take 0.04 to 0.1 s. The first moments after the program starts (about
0.05 s, before `legacy live` begins) are not covered, and a signal there ends the program by
the signal (status 143). Treat exit 0 or the signal after your own SIGTERM as a normal stop,
and send SIGKILL after a grace period (10 s is ample). `cli_supervision` fails a stop that
takes more than 10 s after SIGTERM, and checks that `/api` stops answering while the program is
still running.

Products: an ordinary run on each `.cadu`, not `--finish_processing`:

```
satdump pipeline metop_ahrpt cadu <pass>/live-N/metop_ahrpt.cadu <pass>/products/live-N
```

It exits 0 with `dataset.json`; an empty `.cadu` exits 0 with no usable data, and a missing
one exits 1. Exit codes are those in the table above.

Under these settings SatDump makes no network connection (`cli_offline` proves it in a
network namespace). Avoid running `sdr_probe` on a schedule: it asks every driver, and some
of them search the network.

## Building and testing

On the Pi, or on any arm64 Debian 13 machine (a VM is much faster than the Pi):

```bash
cmake -C cmake/fork/pi.cmake -DSATDUMP_TESTS=ON -S . -B build -G Ninja
tests/gate.sh build            # build, warning check, install into an emptied build/check-prefix, all tests
cmake --install build --prefix ~/satdump    # a relocatable install
```

`tests/gate.sh` fails on any test failure and on any **new** compiler warning. When
warnings are fixed, `tests/gate.sh build --update` lowers the baseline in
`tests/warnings/<arch>.txt`; the baseline should only ever go down. The tests never
contact the network; a few unit tests download from a small server they run on 127.0.0.1.
`cli_thread_faults` uses `taskset` and `chrt` (util-linux) for its one-CPU runs.

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

With `-DSATDUMP_WITH_RUST=ON` added to that configuration, `fuzz_aos_demux` also checks that
the Rust AOS packet splitter gives exactly the C++ one's packets for every frame, and stops at
the first difference, and `fuzz_msumr_lrpt` decodes every packet with both the C++ and the
Rust segment decoder and stops on any difference.

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
  once and switches Rust off for the rest of the run. A Rust part that keeps state from one
  call to the next (the AOS packet splitter) cannot hand that state to the C++ code: when it
  fails, a new C++ object takes over from that input on, and what the Rust one held is lost,
  as after a lost frame. `unit_aos_rust` makes the Rust splitter fail on purpose in the
  middle of a stream to check this.
- `SATDUMP_NO_RUST=1` in the environment switches Rust off; the log says
  `Rust parts: on` or `off` at start-up.
- Each Rust part keeps its C++ twin (for example `derand_ccsds_cpp`, or the C++ class inside
  `src-core/common/ccsds/ccsds_aos/demuxer.cpp`) as the reference: the unit tests compare them
  byte for byte, and the golden pictures are checked with Rust on and
  off. Rust files stay at 500 lines or fewer.
- Ported so far: the CCSDS derandomiser, the CCSDS AOS packet splitter (`rust/src/aos`) and
  the Meteor LRPT MSU-MR segment decoder (`rust/src/lrpt`, called by the meteor_support
  plugin's `Segment` through `rust/bridge/rust_lrpt.h`). In Rust builds `SATDUMP_WITH_RUST` is
  also defined for the plugins and tests that link the core library, so plugin code can call
  a bridge. A part with its own bridge file (`rust_aos.cpp`, `rust_lrpt.cpp`) handles a Rust
  failure there the same way: one log line, and a `CppOnly` guard kept for the rest of the
  run, so every Rust part is off from then on.

Rust tests in the gate: `rust_unit` (cargo test), `rust_clippy` (clippy, warnings are
errors), `cli_rust` (no Rust symbols exported, no extra libraries needed, the switch works,
and the Meteor plugin's `Segment` calls the Rust segment decoder),
`golden_no_rust`, the C++-against-Rust cases in `unit`, `unit_aos_rust` (the AOS packet
splitter with Rust against C++, and a Rust failure in the middle of a stream; a program of
its own, because that test switches Rust off for the rest of the run), and the `diff_*` tests
below.

### Differential tests (C++ against Rust)

Every Rust part has a twin: the C++ code SatDump ships, and the Rust code with the same
buffer contract. Both get the same cases and must give the same outcome (a status, bytes and
records). The tests are built only with `-DSATDUMP_WITH_RUST=ON` and run in the gate's `unit`
label: `diff_selftest`, `diff_twins`, `diff_oracles`, `diff_corpus_make`, `diff_corpus`,
`diff_oracles_make`, `diff_rust_replay` and `diff_lrpt_fallback`.

- Cases come from edge lists, a seeded generator, the golden recordings and fuzz corpora,
  and mutations of these. Stream decoders are fed in chunks, and the chunk boundaries are
  part of each case.
- A twin can read corpus files itself (`corpus_reader`). `lrpt_segment` takes the MSU-MR
  packets out of Meteor CADUs as SatDump does, and cuts any other file into packets as
  `fuzz_msumr_lrpt` does, so a fuzz crash file can be replayed with
  `satdump-diff run lrpt_segment --corpus <file>`.
- `SD_DIFF_SEED` sets the seed. `SD_DIFF_CASES` sets the seeded cases per twin: 2000 by
  default (about a second for all twins), 200000 for a deep run (under a minute). The AOS
  packet splitter twin (`aos_demux`) has about 2230 cases by default; its deep run is
  `SD_DIFF_CASES=1000000 <build>/satdump-diff run aos_demux` (about 36 million frames, 11
  million of them in the 12 layouts SatDump uses; about a minute on the arm64 VM). For a twin
  that sorts its cases into groups (`aos_demux`: by layout), `satdump-diff run` also prints
  the cases and frames of each group.
- The C++ side of a comparison always runs with Rust paused (`CppOnly`), so it stays C++
  even where SatDump calls Rust. An exception or a Rust panic counts as a difference.
- The C++ writes JSONL oracles. Small ones are committed in `tests/diff/oracles`, and
  `cargo test` replays them without the C++.
- Floats: Rust must match the C++ as it ships; a pixel or any other integer never gets a
  tolerance. If a ported C++ file gives other results when floating-point operations are
  contracted, it is built with `-ffp-contract=off` in Rust-on builds only
  (`cmake/fork/rust.cmake`), so Rust-off builds such as the Pi's keep their compile flags,
  and `golden` and `golden_no_rust` are checked. Where the C++ calls `std::fma`, the Rust uses
  `mul_add`. A ULP tolerance is allowed only for float outputs no flag makes exact. A port
  that needs no flag says why in its row (the AOS splitter has no floats; the LRPT decoder's
  quantisation table gives the same integers either way, its time stamp is exact in a double
  and its IDCT uses only integers).
- A difference is saved in `<build>/diff-failures`; one line names the first differing byte
  or record and gives the replay command (`satdump-diff replay <folder>`).
- Fuzz corpora are not in ctest (none are committed); run a fuzz run's corpus folder by hand
  with `satdump-diff run --corpus <folder>`.

### CCSDS AOS packet splitter

With Rust on, every AOS decoder's packets come from the Rust splitter (`rust/src/aos`); it is
3 to 6 times faster than the C++ one on the arm64 VM. A packet zone under 6 or over 65536
bytes, or an insert zone outside 0 to 65536 bytes, stays on the C++ code. Strict mode (the
last constructor argument, off by default; no decoder turns it on yet) also drops a packet
that runs on from an earlier frame when the frame it ends in disagrees with it:

1. if a packet starts in that frame, the old one must end exactly there;
2. if none starts in it, the old one must fill the frame to its end.

The rules are written in `src-core/common/ccsds/ccsds_aos/demuxer.h`, and the C++ and Rust code
apply them the same way.

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
- A SIGTERM in the first moments after `satdump` starts (about 0.05 s, before `legacy live`
  begins) ends the program by the signal (status 143), not with exit 0.
- `sdr_probe` and the GUI ask every radio driver, including those that search the network.
- A partly damaged IERS bulletin stores its readable entries (each date is stored on its own,
  as upstream does and an existing test pins).
- Airspy GUI: the "Manual" gain button checks `gain_type == 22`, so it never shows as selected
  (the GUI only; the command line is not affected).
- `legacy live --client` whose server does not answer cannot be stopped: the network client
  module waits in `nng_recv`, so the pipeline's stop never returns, after a SIGTERM or after a
  web server that cannot start. For the same reason client mode has no command-line test; it
  shares the `--http_server` check with the tested modes.
- Multi-VFO: a VFO whose frequency shift is outside the sample rate ends the program with
  `exit(1)` while the radio, the splitter and the VFOs already started are still running.
- `legacy record` with `--decimation` above 1 never stops its resampler block.
- DVB-S2 `S2PLSyncBlock` starts and joins its second thread outside the block's start/stop
  lock, so a start() and a stop() on two threads can still race on that thread.
- Single (not multi-VFO) `legacy live`: the stop on an error while the radio and the pipeline
  run has no test; nothing known throws there now (multi-VFO's is tested).

## Staying current

`pi` is rebased onto upstream `master` about once a month, and sooner for upstream crash
or security fixes. Small, general fixes may be offered upstream as separate pull requests.
