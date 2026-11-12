#include "demuxer.h"
#include "mpdu.h"
#include "vcdu.h"
#include <algorithm>
#include <cstring>
#include <vector>
#ifdef SATDUMP_WITH_RUST
#include "rust_aos.h"
#endif

#define HEADER_LENGTH 6

namespace ccsds
{
    namespace ccsds_aos
    {
        namespace
        {
            // Fork: the demuxer's C++ code, moved here unchanged from the Demuxer class (which
            // now keeps its state in Demuxer::Impl, below) apart from the strict-mode checks
            // (demuxer.h) and one member that was never used (currentPacketEnd, which clang
            // warned about). In Rust builds it is the reference the Rust demuxer is compared
            // with, and the fallback.
            class CppDemuxer
            {
            private:
                const int MPDU_DATA_SIZE;
                const bool HAS_INSERT_ZONE;
                const int INSERT_ZONE_SIZE;
                const bool SECONDARY_HEADER_EXTENDS_PKT;
                const bool CHECK_FRAME_COUNTER;
                const bool STRICT_MODE; // Fork: strict mode (demuxer.h)

            private:
                CCSDSPacket currentCCSDSPacket;                                                             // Current CCSDS
                std::vector<CCSDSPacket> ccsdsBuffer;                                                       // Buffer to store what we're working on
                void pushPacket();                                                                          // We're done with it, end it
                void abortPacket();                                                                         // Abort this one!
                void readPacket(uint8_t *h);                                                                // Start a new packet
                void pushPayload(uint8_t *data, int length);                                                // Fill a packet up
                int currentPacketPayloadLength, totalPacketLength, remainingPacketLength;                   // Few utils variables
                bool workingOnPacket, inHeader;                                                             // Same
                uint8_t headerBuffer[6];                                                                    // Buffer used to buffer a header overlaping 2 CADUs
                int inHeaderBuffer;                                                                         // Used to fill it up properly
                int lastVCDUCounter;                                                                        // Fork: frame counter of the last CADU, -1 before the first

            public:
                CppDemuxer(int mpdu_data_size, bool hasInsertZone, int insertZoneSize, bool secondaryHeaderExtendsPkt, bool checkFrameCounter, bool strict);
                std::vector<CCSDSPacket> work(uint8_t *cadu); // Main function
            };

            CppDemuxer::CppDemuxer(int mpdu_data_size, bool hasInsertZone, int insertZoneSize, bool secondaryHeaderExtendsPkt, bool checkFrameCounter, bool strict)
                : MPDU_DATA_SIZE(mpdu_data_size), HAS_INSERT_ZONE(hasInsertZone), INSERT_ZONE_SIZE(insertZoneSize), SECONDARY_HEADER_EXTENDS_PKT(secondaryHeaderExtendsPkt), CHECK_FRAME_COUNTER(checkFrameCounter), STRICT_MODE(strict)
            {
                // Init variables
                currentCCSDSPacket.header.packet_length = 0;
                currentPacketPayloadLength = 0;
                remainingPacketLength = 0;
                totalPacketLength = 0;
                inHeader = false;
                inHeaderBuffer = 0;
                lastVCDUCounter = -1;
                abortPacket();
            }

            // Fill header and packet lengths
            void CppDemuxer::readPacket(uint8_t *h)
            {
                workingOnPacket = true;
                currentCCSDSPacket.payload.clear(); // Fork: a new packet never starts with bytes left from an earlier one
                currentCCSDSPacket.header = parseCCSDSHeader(h);
                currentPacketPayloadLength = currentCCSDSPacket.header.packet_length + 1 + (SECONDARY_HEADER_EXTENDS_PKT ? (currentCCSDSPacket.header.secondary_header_flag ? 8 : 0) : 0);
                totalPacketLength = currentPacketPayloadLength + HEADER_LENGTH;
                remainingPacketLength = currentPacketPayloadLength;
            }

            // Clear everything and push into packet buffer
            void CppDemuxer::pushPacket()
            {
                ccsdsBuffer.push_back(currentCCSDSPacket);
                currentCCSDSPacket.payload.clear();
                currentCCSDSPacket.header.packet_length = 0;
                currentPacketPayloadLength = 0;
                remainingPacketLength = 0;
                workingOnPacket = false;
            }

            // Push data into the packet
            void CppDemuxer::pushPayload(uint8_t *data, int length)
            {
                for (int i = 0; i < length; i++)
                    currentCCSDSPacket.payload.push_back(data[i]);

                remainingPacketLength -= length;
            }

            // ABORT!
            void CppDemuxer::abortPacket()
            {
                workingOnPacket = false;
                currentCCSDSPacket.payload.clear();
                currentCCSDSPacket.header.packet_length = 0;
                currentPacketPayloadLength = 0;
                remainingPacketLength = 0;
            };

            std::vector<CCSDSPacket> CppDemuxer::work(uint8_t *cadu)
            {
                ccsdsBuffer.clear(); // Clear buffer from previous run

                // Fork: a packet that ended exactly at the end of the previous frame is complete but has
                // not been pushed yet (it used to be pushed further down, still before any other
                // packet of this frame). Push it first, so the checks below only drop unfinished ones.
                if (remainingPacketLength == 0 && workingOnPacket)
                    pushPacket();

                // Fork: a lost frame went unnoticed, so the packet in progress was finished with
                // bytes from a later frame. A demuxer sees one virtual channel, whose 24-bit frame
                // counter goes up by 1 per frame (and wraps); any other step means frames are
                // missing, so the packet in progress (or a header cut across frames) is dropped and
                // the next packet that starts here is read normally.
                // Only callers that ask for it (MetOp) get this check: other missions' counters,
                // Meteor's included, were not checked against real recordings.
                if (CHECK_FRAME_COUNTER)
                {
                    const uint32_t vcdu_counter = parseVCDU(cadu).vcdu_counter;
                    if (lastVCDUCounter >= 0 && ((vcdu_counter - uint32_t(lastVCDUCounter)) & 0xFFFFFF) != 1)
                    {
                        abortPacket();
                        inHeader = false;
                    }
                    lastVCDUCounter = int(vcdu_counter);
                }

                MPDU mpdu = parseMPDU(cadu, HAS_INSERT_ZONE, INSERT_ZONE_SIZE); // Parse M-PDU Header

                // Sanity check, if the first header point points outside the data payload
                if (mpdu.first_header_pointer < 2047 && mpdu.first_header_pointer >= MPDU_DATA_SIZE)
                {
                    // Fork: this frame's data cannot be used, so the packet in progress would miss
                    // it and be finished with bytes from the next frame: drop it, as for a lost frame
                    abortPacket();
                    inHeader = false;
                    return ccsdsBuffer;
                }

                // If we're in a header or so,
                // we may need to read with an offset
                int offset = 0;

                // We're parsing a header!
                if (inHeader)
                {
                    inHeader = false;

                    // Copy the remaining data into our header
                    std::memcpy(&headerBuffer[inHeaderBuffer], &mpdu.data[0], 6 - inHeaderBuffer);
                    offset = 6 - inHeaderBuffer;
                    inHeaderBuffer += 6 - inHeaderBuffer;

                    // Parse it
                    readPacket(headerBuffer);
                }

                // Are we working on a packet right now? Still got stuff to write? Then do it!
                if (remainingPacketLength > 0 && workingOnPacket)
                {
                    // If there's a header, it takes priority
                    if (mpdu.first_header_pointer < 2047)
                    {
                        // Finish filling up that packet
                        // Fork: the packet has to end before the new header. When it did not, the old
                        // code wrote first_header_pointer + 1 - offset bytes (one byte of the new
                        // header too, or a negative count when offset > first_header_pointer + 1) and
                        // pushed the cut-off packet as if it were whole. Such a packet is dropped now;
                        // one that fits is written in full, as before.
                        // Fork: strict mode also drops one that ends before the new header (rule 1 in demuxer.h)
                        if (STRICT_MODE && remainingPacketLength != int(mpdu.first_header_pointer) - offset)
                            abortPacket();
                        else if (remainingPacketLength <= int(mpdu.first_header_pointer) - offset)
                            pushPayload(&mpdu.data[offset], remainingPacketLength);
                        else
                            abortPacket();
                    }
                    else
                    {
                        // Write what we have
                        // Fork: offset was counted twice here, so after a header cut across frames
                        // up to 4 bytes past the end of the packet could be written. The remaining
                        // length went below 0, the packet was never pushed, and its bytes were put in
                        // front of the next packet. The space left in this frame is MPDU_DATA_SIZE - offset.
                        int toWrite = std::min(remainingPacketLength, MPDU_DATA_SIZE - offset);
                        // Fork: no packet starts in this frame, so strict mode drops one that ends
                        // inside it (rule 2 in demuxer.h)
                        if (STRICT_MODE && remainingPacketLength < MPDU_DATA_SIZE - offset)
                            abortPacket();
                        else
                            pushPayload(&mpdu.data[offset], toWrite);
                    }
                }

                // Are we done writing a packet? Push it
                if (remainingPacketLength == 0 && workingOnPacket)
                {
                    pushPacket();
                }

                // Is there a new header here?
                if (mpdu.first_header_pointer < 2047)
                {
                    // The full header fits
                    if (mpdu.first_header_pointer + HEADER_LENGTH < MPDU_DATA_SIZE)
                    {
                        // Parse it
                        readPacket(&mpdu.data[mpdu.first_header_pointer]);

                        // Compute possible secondary headers in that packet
                        bool hasSecondHeader = MPDU_DATA_SIZE > mpdu.first_header_pointer + totalPacketLength;

                        // A second header can fit, so search for it!
                        if (hasSecondHeader)
                        {
                            // Write first packet
                            if (mpdu.first_header_pointer + totalPacketLength < MPDU_DATA_SIZE)
                            {
                                pushPayload(&mpdu.data[mpdu.first_header_pointer + 6], currentPacketPayloadLength);
                                pushPacket();
                            }
                            else
                            {
                                abortPacket();
                            }

                            // Compute next possible header
                            int nextHeaderPointer = mpdu.first_header_pointer + totalPacketLength;

                            while (nextHeaderPointer < MPDU_DATA_SIZE)
                            {
                                // The header fits!
                                if (nextHeaderPointer + HEADER_LENGTH < MPDU_DATA_SIZE)
                                {
                                    readPacket(&mpdu.data[nextHeaderPointer]);

                                    int toWrite = remainingPacketLength > (MPDU_DATA_SIZE - (nextHeaderPointer + 6)) ? (MPDU_DATA_SIZE - (nextHeaderPointer + 6)) : remainingPacketLength;
                                    pushPayload(&mpdu.data[nextHeaderPointer + 6], toWrite);
                                }
                                // Only part of the header fits. At least 1 byte has to be there or it'll be in the next frame
                                else if (nextHeaderPointer < MPDU_DATA_SIZE)
                                {
                                    inHeader = true;
                                    inHeaderBuffer = 0;
                                    std::memcpy(headerBuffer, &mpdu.data[nextHeaderPointer], MPDU_DATA_SIZE - nextHeaderPointer);
                                    inHeaderBuffer += MPDU_DATA_SIZE - nextHeaderPointer;
                                    break;
                                }

                                // That packet is done? Write it
                                if (remainingPacketLength == 0 && workingOnPacket)
                                {
                                    pushPacket();
                                }

                                // Update to next pointer
                                nextHeaderPointer = nextHeaderPointer + totalPacketLength;
                            }
                        }
                        else
                        {
                            // Sanity check
                            if (workingOnPacket)
                            {
                                // Fill that packet
                                int toWrite = remainingPacketLength > (MPDU_DATA_SIZE - (mpdu.first_header_pointer + 6)) ? (MPDU_DATA_SIZE - (mpdu.first_header_pointer + 6)) : remainingPacketLength;
                                pushPayload(&mpdu.data[mpdu.first_header_pointer + 6], toWrite);
                            }
                        }
                    }
                    else if (mpdu.first_header_pointer < MPDU_DATA_SIZE)
                    {
                        // The header doesn't fit, so fill what we have
                        inHeader = true;
                        inHeaderBuffer = 0;
                        std::memcpy(headerBuffer, &mpdu.data[mpdu.first_header_pointer], MPDU_DATA_SIZE - mpdu.first_header_pointer);
                        inHeaderBuffer += MPDU_DATA_SIZE - mpdu.first_header_pointer;
                    }
                }

                return ccsdsBuffer;
            } // namespace libccsds
        } // namespace

        // Fork: what a Demuxer holds. The C++ demuxer always exists; while the Rust one is in use
        // it is never fed, so it is a fresh demuxer if it has to take over.
        class Demuxer::Impl
        {
        public:
            Impl(int mpdu_data_size, bool hasInsertZone, int insertZoneSize, bool secondaryHeaderExtendsPkt, bool checkFrameCounter, bool strict)
                : cpp(mpdu_data_size, hasInsertZone, insertZoneSize, secondaryHeaderExtendsPkt, checkFrameCounter, strict)
            {
#ifdef SATDUMP_WITH_RUST
                // Null when Rust is off or the Rust code does not take these settings
                rust = satdump::rust::AosDemuxer::create(mpdu_data_size, hasInsertZone, insertZoneSize, secondaryHeaderExtendsPkt, checkFrameCounter, strict);
#endif
            }

            std::vector<CCSDSPacket> work(uint8_t *cadu)
            {
#ifdef SATDUMP_WITH_RUST
                if (rust)
                {
                    std::vector<CCSDSPacket> packets;
                    if (rust->work(cadu, packets))
                        return packets;
                    // Rust failed on this CADU (it was logged, and Rust is off from now on), or Rust
                    // was switched off. Nothing of this CADU was used, and what the Rust demuxer held
                    // is lost, as after a lost frame: the C++ demuxer starts with this CADU.
                    rust.reset();
                }
#endif
                return cpp.work(cadu);
            }

        private:
            CppDemuxer cpp;
#ifdef SATDUMP_WITH_RUST
            std::unique_ptr<satdump::rust::AosDemuxer> rust;
#endif
        };

        Demuxer::Demuxer(int mpdu_data_size, bool hasInsertZone, int insertZoneSize, bool secondaryHeaderExtendsPkt, bool checkFrameCounter, bool strict)
            : impl(std::make_unique<Impl>(mpdu_data_size, hasInsertZone, insertZoneSize, secondaryHeaderExtendsPkt, checkFrameCounter, strict))
        {
        }

        Demuxer::~Demuxer() = default;
        Demuxer::Demuxer(Demuxer &&other) noexcept = default;
        Demuxer &Demuxer::operator=(Demuxer &&other) noexcept = default;

        std::vector<CCSDSPacket> Demuxer::work(uint8_t *cadu) { return impl->work(cadu); }
    } // namespace ccsds_aos
} // namespace ccsds
