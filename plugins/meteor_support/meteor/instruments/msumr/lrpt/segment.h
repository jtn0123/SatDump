#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace meteor
{
    namespace msumr
    {
        namespace lrpt
        {
            class Segment
            {
            public:
                // Fork: every header field starts at a known value. The default constructor
                // (std::map::operator[] runs it for every missing segment) and a packet with no
                // picture data after its 14-byte header left them unset, and isValid() read them
                // anyway. A timestamp of -1 is what the reader already uses for "no time".
                uint16_t day_time = 0;
                uint32_t ms_time = 0;
                uint16_t us_time = 0;
                double timestamp = -1;

                uint8_t MCUN = 0;
                uint8_t QT = 0;
                uint8_t DC = 0;
                uint8_t AC = 0;
                uint16_t QFM = 0;
                uint8_t QF = 0;
                bool valid;
                bool partial;
                uint8_t lines[8][14 * 8] = {{0}};

                bool isValid();

                Segment();
                Segment(uint8_t *data, int length, bool partial, bool meteorm2x_mode);
                ~Segment();

                void decode(uint8_t *data, int length);
            };
        } // namespace lrpt
    }     // namespace msumr
} // namespace meteor