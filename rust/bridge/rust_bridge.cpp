// Fork: the C++ side of SatDump's Rust parts; see rust_bridge.h and rust/src/ffi.rs.
#include "rust_bridge.h"
#include "logger.h"
#include <atomic>
#include <cstdlib>
#include <cstring>

extern "C"
{
    int32_t satdump_rs_derand_ccsds(uint8_t *data, int32_t length);
    int32_t satdump_rs_derand_ccsds_soft(int8_t *data, int32_t length);
    int32_t satdump_rs_derand_ccsds_bits(uint8_t *data, int32_t length);
    int32_t satdump_rs_panic_check();
}

namespace satdump::rust
{
    namespace
    {
        bool allowed_by_environment()
        {
            const char *v = std::getenv("SATDUMP_NO_RUST");
            return v == nullptr || v[0] == '\0' || std::strcmp(v, "0") == 0;
        }

        std::atomic<bool> rust_on{allowed_by_environment()};
        std::atomic<int> paused{0}; // CppOnly guards that exist

        // True when Rust did the work. After a Rust failure the C++ code does it from then on.
        bool done(int32_t status, const char *part)
        {
            if (status == STATUS_OK)
                return true;
            if (status != STATUS_BAD_ARGUMENT && rust_on.exchange(false) && logger)
                logger->error("The Rust %s failed (status %d); SatDump uses its C++ code for the rest of this run", part, (int)status);
            return false;
        }
    } // namespace

    bool enabled() { return rust_on.load(std::memory_order_relaxed) && paused.load(std::memory_order_relaxed) == 0; }

    bool derand_ccsds(uint8_t *data, int length)
    {
        if (!enabled())
            return false;
        return done(satdump_rs_derand_ccsds(data, length), "derandomiser");
    }

    bool derand_ccsds_soft(int8_t *data, int length)
    {
        if (!enabled())
            return false;
        return done(satdump_rs_derand_ccsds_soft(data, length), "soft-symbol derandomiser");
    }

    bool derand_ccsds_bits(uint8_t *data, int length)
    {
        if (!enabled())
            return false;
        return done(satdump_rs_derand_ccsds_bits(data, length), "bit derandomiser");
    }

    int panic_check() { return satdump_rs_panic_check(); }

    CppOnly::CppOnly() { paused.fetch_add(1); }
    CppOnly::~CppOnly() { paused.fetch_sub(1); }
} // namespace satdump::rust
