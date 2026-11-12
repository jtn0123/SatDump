# MQTT-C (bundled copy)

A small MQTT 3.1.1 client library in C, used by `src-core/utils/mqtt_client.cpp` (and through
it by the Elektro-Arktika GGAK modules and tools).

Version: 1.1.7
Upstream: https://github.com/LiamBindle/MQTT-C
Upstream commit: 102e59c14dd0e01770b357a462bd462c83de62e2 (tag v1.1.7, 2026-10-29)
License: MIT (`LICENSE`, copied from the same commit)

## Files

| File | Upstream file |
|---|---|
| `mqtt.c` | `src/mqtt.c` |
| `mqtt_pal.c` | `src/mqtt_pal.c` |
| `mqtt.h` | `include/mqtt.h` |
| `mqtt_pal.h` | `include/mqtt_pal.h` |
| `posix_sockets.h` | `examples/templates/posix_sockets.h` (unchanged upstream since 1.1.6) |
| `LICENSE` | `LICENSE` |

Only these files are kept; SatDump compiles `mqtt.c` and `mqtt_pal.c` (the plain socket
platform layer, no TLS).

## Local changes

Re-apply these after copying in a new upstream version:

1. `mqtt.c`, `mqtt_pal.c`: `#include "mqtt.h"` instead of `#include <mqtt.h>`, and `mqtt.h`:
   `#include "mqtt_pal.h"` instead of `#include <mqtt_pal.h>`, so the headers are found next to
   the sources without an extra include path (SatDump, 2026-07).
2. `posix_sockets.h`, for Windows (SatDump, 2026-08): `<unistd.h>` is included only outside
   Windows, a failed connect closes the socket with `closesocket()` on Windows, and the
   `ioctlsocket()` argument is a `u_long`.
3. `posix_sockets.h` (fork, 2026-11): the `addrinfo` hints are cleared with `memset` instead of
   `= {0}`, which C++ compilers warn about (`-Wmissing-field-initializers`).

## History

- 2026-07: SatDump imported upstream master at commit 7a986a6 (2022-10-27, after v1.1.6).
- 2026-11 (fork): updated to v1.1.7, which fixes CVE-2026-54412 (a PUBLISH whose topic length
  runs past the packet was read out of bounds in `mqtt_unpack_publish_response`), an
  out-of-bounds read in `mqtt_error_str`, message-queue alignment, and `send_offset` on
  `mqtt_reinit`. Tests: `tests/unit/test_mqtt.cpp`.
