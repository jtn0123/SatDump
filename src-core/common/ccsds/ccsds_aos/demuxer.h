#pragma once

#include "../ccsds.h"
#include <vector>

namespace ccsds
{
    namespace ccsds_aos
    {
        /*
        Simple CCSDS demuxer building CCSDS frames from CADUs.
        VCID filtering must be done beforehand!
        Fork: with checkFrameCounter, each demuxer follows the frame counter of its one virtual
        channel and drops the packet in progress when frames are missing (see work()).
    */
        class Demuxer
        {
        private:
            const int MPDU_DATA_SIZE;
            const bool HAS_INSERT_ZONE;
            const int INSERT_ZONE_SIZE;
            const bool SECONDARY_HEADER_EXTENDS_PKT;
            const bool CHECK_FRAME_COUNTER;

        private:
            CCSDSPacket currentCCSDSPacket;                                                             // Current CCSDS
            std::vector<CCSDSPacket> ccsdsBuffer;                                                       // Buffer to store what we're working on
            void pushPacket();                                                                          // We're done with it, end it
            void abortPacket();                                                                         // Abort this one!
            void readPacket(uint8_t *h);                                                                // Start a new packet
            void pushPayload(uint8_t *data, int length);                                                // Fill a packet up
            int currentPacketPayloadLength, totalPacketLength, remainingPacketLength, currentPacketEnd; // Few utils variables
            bool workingOnPacket, inHeader;                                                             // Same
            uint8_t headerBuffer[6];                                                                    // Buffer used to buffer a header overlaping 2 CADUs
            int inHeaderBuffer;                                                                         // Used to fill it up properly
            int lastVCDUCounter;                                                                        // Fork: frame counter of the last CADU, -1 before the first

        public:
            Demuxer(int mpdu_data_size = 884, bool hasInsertZone = false, int insertZoneSize = 2, bool secondaryHeaderExtendsPkt = false, bool checkFrameCounter = false);
            std::vector<CCSDSPacket> work(uint8_t *cadu); // Main function
        };
    } // namespace ccsds_aos
} // namespace ccsds