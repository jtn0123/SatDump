// Fork (M6.3): the C++ side of the Rust Meteor LRPT segment decoder; see rust_lrpt.h and
// rust/src/ffi_lrpt.rs.
#include "rust_lrpt.h"
#include "logger.h"
#include "rust_bridge.h"
#include <cstddef>
#include <mutex>

extern "C"
{
    int32_t satdump_rs_lrpt_segment(const uint8_t *data, int32_t length, uint8_t partial, uint8_t m2x,
                                    satdump::rust::LrptSegment *out);
    int32_t satdump_rs_lrpt_quant_table(int32_t qf, int64_t *table);
    void satdump_rs_lrpt_fail_next_segment(int32_t block);
}

namespace satdump::rust
{
    // The places rust/src/ffi_lrpt.rs checks too
    static_assert(sizeof(LrptSegment) == 928, "LrptSegment must have the layout of the Rust one");
    static_assert(offsetof(LrptSegment, ms_time) == 8 && offsetof(LrptSegment, qfm) == 16 &&
                      offsetof(LrptSegment, qf) == 22 && offsetof(LrptSegment, partial) == 24 &&
                      offsetof(LrptSegment, lines) == 25,
                  "LrptSegment must have the layout of the Rust one");

    namespace
    {
        std::once_flag failure_logged;

        void log_failure(int32_t status, const char *part)
        {
            if (logger)
                logger->error("The Rust %s failed (status %d); SatDump uses its C++ code for the rest of this run", part, (int)status);
        }

        // True when Rust did the work. A bad argument changed nothing and is not a Rust failure.
        // Any other status is one: logged once, and Rust is switched off for the rest of the run.
        // rust_bridge.cpp keeps its own switch to itself, so this keeps Rust paused instead, as a
        // CppOnly guard does, until SatDump exits (as the AOS demuxer's bridge does); enabled()
        // is then false, as after a failure there.
        bool finished(int32_t status, const char *part)
        {
            if (status == STATUS_OK)
                return true;
            if (status != STATUS_BAD_ARGUMENT)
            {
                static const CppOnly switched_off; // made by the first failure (thread-safe)
                std::call_once(failure_logged, log_failure, status, part);
            }
            return false;
        }
    } // namespace

    bool lrpt_segment(const uint8_t *data, int length, bool partial, bool m2x, LrptSegment *out)
    {
        if (!enabled())
            return false;
        return finished(satdump_rs_lrpt_segment(data, length, partial, m2x, out), "Meteor LRPT segment decoder");
    }

    bool lrpt_quant_table(int qf, int64_t table[64])
    {
        if (!enabled())
            return false;
        return finished(satdump_rs_lrpt_quant_table(qf, table), "Meteor LRPT quantisation table");
    }

    void lrpt_fail_next_segment(int block) { satdump_rs_lrpt_fail_next_segment(block); }
} // namespace satdump::rust
