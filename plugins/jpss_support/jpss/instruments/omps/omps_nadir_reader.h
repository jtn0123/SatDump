#pragma once

#include "common/ccsds/ccsds.h"
#include <cmath>
#include "image/image.h"
// Fork: the system libaec on Linux, the bundled copy elsewhere (cmake/fork/codecs.cmake)
extern "C"
{
#include <szlib.h>
}

namespace jpss
{
    namespace omps
    {
        class OMPSNadirReader
        {
        private:
            std::vector<uint16_t> channels[339];
            uint8_t *finalFrameVector;
            std::vector<uint8_t> currentOMPSFrame;
            SZ_com_t rice_parameters;

        public:
            OMPSNadirReader();
            ~OMPSNadirReader();
            int lines;
            std::vector<double> timestamps;
            void work(ccsds::CCSDSPacket &packet);
            image::Image getChannel(int channel);
        };
    } // namespace atms
} // namespace jpss