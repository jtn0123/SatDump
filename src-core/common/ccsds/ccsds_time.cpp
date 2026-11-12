#include "ccsds_time.h"

namespace ccsds
{
    // Fork: each parser returns -1 (the usual "no time" value) when the packet is too short,
    // and builds 32-bit fields from unsigned bytes. parseCCSDSTime works in 64 bits, so large
    // day counts (corrupt data, or dates after 2038) no longer overflow.
    static uint32_t be32(const uint8_t *p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]); }

    time_t parseCCSDSTime(CCSDSPacket &pkt, int offset, int ms_scale)
    {
        if (pkt.payload.size() < 6)
            return -1;
        uint16_t days = pkt.payload[0] << 8 | pkt.payload[1];
        uint32_t milliseconds_of_day = be32(&pkt.payload[2]);

        return (time_t(offset) + days) * 86400 + milliseconds_of_day / ms_scale;
    }

    double parseCCSDSTimeFull(CCSDSPacket &pkt, int offset, int ms_scale, int us_of_ms_scale)
    {
        if (pkt.payload.size() < 8)
            return -1;
        uint16_t days = pkt.payload[0] << 8 | pkt.payload[1];
        uint32_t milliseconds_of_day = be32(&pkt.payload[2]);
        uint16_t microseconds_of_millisecond = pkt.payload[6] << 8 | pkt.payload[7];

        return double(offset + days) * 86400.0 + double(milliseconds_of_day) / double(ms_scale) + double(microseconds_of_millisecond) / double(us_of_ms_scale);
    }

    double parseCCSDSTimeFullRaw(uint8_t *data, int offset, int ms_scale, int us_of_ms_scale)
    {
        uint16_t days = data[0] << 8 | data[1];
        uint32_t milliseconds_of_day = be32(&data[2]);
        uint16_t microseconds_of_millisecond = data[6] << 8 | data[7];

        return double(offset + days) * 86400.0 + double(milliseconds_of_day) / double(ms_scale) + double(microseconds_of_millisecond) / double(us_of_ms_scale);
    }

    double parseCCSDSTimeFullRawUnsegmented(uint8_t *data, int offset, double ms_scale)
    {
        uint8_t seconds_to_convert = data[1] & 0b1111111;
        uint32_t seconds_since_epoch = be32(&data[2]);
        uint16_t subsecond_time = data[6] << 8 | data[7];

        return offset * 86400.0 + double(seconds_since_epoch - seconds_to_convert) + double(subsecond_time) * double(ms_scale);
    }
}