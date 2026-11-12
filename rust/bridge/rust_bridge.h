#pragma once

// Fork: C++ functions for SatDump's Rust parts (rust/). Built only with -DSATDUMP_WITH_RUST=ON.
//
// The Rust code stays hidden inside libsatdump_core.so, so plugins and the rest of SatDump
// reach it only through these functions. Each returns false when Rust did not do the work:
// Rust is switched off (SATDUMP_NO_RUST=1), an argument was bad, or the Rust code failed.
// The buffer is then unchanged and the caller runs its C++ code instead. A Rust failure is
// logged once and switches Rust off for the rest of the run.

#include <cstdint>

namespace satdump::rust
{
    // Status codes, as in rust/src/ffi.rs
    constexpr int STATUS_OK = 0;
    constexpr int STATUS_BAD_ARGUMENT = 1;
    constexpr int STATUS_PANIC = 2;

    /// Rust is in use: SATDUMP_NO_RUST is not set and no Rust part has failed in this run
    bool enabled();

    /// CCSDS derandomiser, in place (the twin of derand_ccsds_cpp)
    bool derand_ccsds(uint8_t *data, int length);

    /// CCSDS derandomiser on soft symbols, in place (the twin of derand_ccsds_soft). Not used by
    /// SatDump yet: the differential tests (tests/diff) compare it with the C++ one first.
    bool derand_ccsds_soft(int8_t *data, int length);

    /// CCSDS derandomiser on one bit per byte, in place (the twin of derand_ccsds_bits). Not used
    /// by SatDump yet, as derand_ccsds_soft.
    bool derand_ccsds_bits(uint8_t *data, int length);

    /// Makes the Rust code panic on purpose and returns the status it reports (STATUS_PANIC);
    /// for tests. It does not switch Rust off.
    int panic_check();

    /// While one of these exists, Rust is paused: enabled() is false and the functions above
    /// leave the work to the C++ code, as with SATDUMP_NO_RUST. For tests: the differential
    /// tests (tests/diff) run the C++ side of each comparison under one, so that side stays the
    /// C++ code even where SatDump calls Rust.
    class CppOnly
    {
    public:
        CppOnly();
        ~CppOnly();
        CppOnly(const CppOnly &) = delete;
        CppOnly &operator=(const CppOnly &) = delete;
    };
} // namespace satdump::rust
