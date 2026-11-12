#pragma once

// Fork: the Rust CCSDS AOS demuxer (rust/src/aos), for ccsds::ccsds_aos::Demuxer
// (src-core/common/ccsds/ccsds_aos/demuxer.h), which uses it in Rust builds. Built only with
// -DSATDUMP_WITH_RUST=ON; see rust_bridge.h for the rules.
//
// The demuxer has state, so a Rust failure cannot simply be redone by the C++ code: work()
// then returns false, the failure is logged once and Rust is switched off for the rest of
// the run, and the caller hands this CADU and the ones after it to a new C++ demuxer. The
// packet in progress in the Rust demuxer is lost, as after a lost frame.

#include "common/ccsds/ccsds.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace satdump::rust
{
    class AosDemuxer
    {
    public:
        /// A Rust demuxer with the settings of the C++ one, or null when Rust is not in use
        /// (enabled() is false), the Rust code does not take these settings (see supports()),
        /// or it failed (which switches Rust off)
        static std::unique_ptr<AosDemuxer> create(int mpdu_data_size, bool hasInsertZone, int insertZoneSize,
                                                  bool secondaryHeaderExtendsPkt, bool checkFrameCounter, bool strict);
        /// Whether the Rust code takes these settings: a packet zone of 6 to 65536 bytes and,
        /// when there is an insert zone, one of 0 to 65536 bytes
        static bool supports(int mpdu_data_size, bool hasInsertZone, int insertZoneSize);

        ~AosDemuxer();
        AosDemuxer(const AosDemuxer &) = delete;
        AosDemuxer &operator=(const AosDemuxer &) = delete;

        /// The packets that end in this CADU, added to `packets`, as the C++ demuxer's work()
        /// gives them. `cadu` starts at the sync marker and has at least cadu_bytes() bytes.
        /// False when Rust did not do the work (Rust was switched off, or it failed: see
        /// above); `packets` is then as it was.
        bool work(const uint8_t *cadu, std::vector<ccsds::CCSDSPacket> &packets);

        /// The bytes of a CADU that work() reads: up to the end of the packet zone
        size_t cadu_bytes() const { return bytes; }

        /// For tests: the `cadus`-th work() call from now, on any Rust demuxer (1: the next
        /// one), fails as if the Rust code had panicked (it does panic); 0 cancels it. This
        /// hook (and satdump_rs_aos_panic_in in rust/src/ffi_aos.rs) is in the library SatDump
        /// runs on purpose, as panic_check() in rust_bridge.h is: the tests then check the
        /// fallback of the code SatDump uses, not of a copy built for them. Nothing else calls
        /// it, and it costs one relaxed atomic read per CADU.
        static void fail_after(int cadus);

    private:
        struct Handle;
        AosDemuxer(Handle *handle, size_t bytes) : handle(handle), bytes(bytes) {}
        Handle *handle;
        size_t bytes;
    };
} // namespace satdump::rust
