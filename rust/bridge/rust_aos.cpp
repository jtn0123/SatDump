// Fork: the C++ side of the Rust CCSDS AOS demuxer; see rust_aos.h and rust/src/ffi_aos.rs.
#include "rust_aos.h"
#include "logger.h"
#include "rust_bridge.h"
#include <atomic>
#include <mutex>

extern "C"
{
    struct satdump_rs_aos;
    int32_t satdump_rs_aos_new(int32_t mpdu_data_size, bool has_insert_zone, int32_t insert_zone_size, bool secondary_header_extends_pkt,
                               bool check_frame_counter, bool strict, satdump_rs_aos **out, int32_t *cadu_bytes);
    int32_t satdump_rs_aos_free(satdump_rs_aos *handle);
    int32_t satdump_rs_aos_work(satdump_rs_aos *handle, const uint8_t *cadu, int32_t length, int32_t *count);
    int32_t satdump_rs_aos_packet(const satdump_rs_aos *handle, int32_t index, uint8_t *header, const uint8_t **payload, int32_t *length);
    int32_t satdump_rs_aos_panic_in(satdump_rs_aos *handle, int32_t calls);
}

namespace satdump::rust
{
    struct AosDemuxer::Handle
    {
        satdump_rs_aos *rs;
    };

    namespace
    {
        // The limits of rust/src/aos/mod.rs (Settings::new)
        constexpr int MIN_MPDU_DATA_SIZE = 6, MAX_MPDU_DATA_SIZE = 65536, MAX_INSERT_ZONE_SIZE = 65536;

        std::atomic<int> fail_countdown{0}; // fail_after()
        std::once_flag failure_logged;

        void log_failure(int32_t status, const char *what)
        {
            if (logger)
                logger->error("The Rust AOS demuxer failed (%s, status %d); SatDump uses its C++ code for the rest of this run", what, (int)status);
        }

        // A Rust failure: logged once, and Rust is switched off for the rest of the run.
        // rust_bridge.cpp keeps its own switch to itself, so this keeps Rust paused instead, as
        // a CppOnly guard does, until SatDump exits; enabled() is then false, as after a
        // failure there.
        void failed(int32_t status, const char *what)
        {
            static const CppOnly switched_off; // made by the first failure (thread-safe)
            std::call_once(failure_logged, log_failure, status, what);
        }
    } // namespace

    bool AosDemuxer::supports(int mpdu_data_size, bool hasInsertZone, int insertZoneSize)
    {
        const int insert_zone = hasInsertZone ? insertZoneSize : 0;
        return mpdu_data_size >= MIN_MPDU_DATA_SIZE && mpdu_data_size <= MAX_MPDU_DATA_SIZE && insert_zone >= 0 &&
               insert_zone <= MAX_INSERT_ZONE_SIZE;
    }

    std::unique_ptr<AosDemuxer> AosDemuxer::create(int mpdu_data_size, bool hasInsertZone, int insertZoneSize,
                                                   bool secondaryHeaderExtendsPkt, bool checkFrameCounter, bool strict)
    {
        if (!enabled() || !supports(mpdu_data_size, hasInsertZone, insertZoneSize))
            return nullptr;
        satdump_rs_aos *rs = nullptr;
        int32_t bytes = 0;
        const int32_t status = satdump_rs_aos_new(mpdu_data_size, hasInsertZone, insertZoneSize, secondaryHeaderExtendsPkt,
                                                  checkFrameCounter, strict, &rs, &bytes);
        if (status != STATUS_OK)
        {
            if (status != STATUS_BAD_ARGUMENT)
                failed(status, "making a demuxer");
            return nullptr;
        }
        return std::unique_ptr<AosDemuxer>(new AosDemuxer(new Handle{rs}, size_t(bytes)));
    }

    AosDemuxer::~AosDemuxer()
    {
        satdump_rs_aos_free(handle->rs);
        delete handle;
    }

    bool AosDemuxer::work(const uint8_t *cadu, std::vector<ccsds::CCSDSPacket> &packets)
    {
        if (!enabled())
            return false;
        // Test hook: the countdown's last call makes this one panic
        if (fail_countdown.load(std::memory_order_relaxed) > 0 && fail_countdown.fetch_sub(1) == 1)
            satdump_rs_aos_panic_in(handle->rs, 1);

        int32_t count = 0;
        const int32_t status = satdump_rs_aos_work(handle->rs, cadu, int32_t(bytes), &count);
        if (status != STATUS_OK)
        {
            // A bad argument (a null CADU) changed nothing and is not a Rust failure
            if (status != STATUS_BAD_ARGUMENT)
                failed(status, "on a CADU");
            return false;
        }
        const size_t before = packets.size();
        packets.reserve(before + size_t(count));
        for (int32_t i = 0; i < count; i++)
        {
            uint8_t header[6];
            const uint8_t *payload = nullptr;
            int32_t length = 0;
            const int32_t got = satdump_rs_aos_packet(handle->rs, i, header, &payload, &length);
            if (got != STATUS_OK)
            {
                packets.erase(packets.begin() + before, packets.end());
                failed(got, "reading a packet");
                return false;
            }
            ccsds::CCSDSPacket &p = packets.emplace_back();
            p.header = ccsds::parseCCSDSHeader(header);
            p.payload.assign(payload, payload + length);
        }
        return true;
    }

    void AosDemuxer::fail_after(int cadus) { fail_countdown.store(cadus > 0 ? cadus : 0); }
} // namespace satdump::rust
