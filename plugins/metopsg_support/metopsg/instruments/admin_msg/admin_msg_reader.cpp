#include "admin_msg_reader.h"
#include "logger.h"
#include <bzlib.h> // Fork: the system bzip2 on Linux, the bundled copy elsewhere (cmake/fork/codecs.cmake)
#include <fstream>

namespace metopsg
{
    namespace admin_msg
    {
        AdminMsgReader::AdminMsgReader()
        {
            count = 0;
            message_out = new uint8_t[MAX_MSG_SIZE];
        }

        AdminMsgReader::~AdminMsgReader() { delete[] message_out; }

        void AdminMsgReader::work(ccsds::CCSDSPacket &packet)
        {
            // Fork: the bzip2 stream starts after 39 bytes; a shorter packet made the length wrap around
            if (packet.payload.size() <= HEADER_SIZE)
            {
                logger->warn("Admin message packet too short (%d bytes), skipping", (int)packet.payload.size());
                return;
            }

            unsigned int outsize = MAX_MSG_SIZE;

            int ret = BZ2_bzBuffToBuffDecompress((char *)message_out, &outsize, (char *)&packet.payload[HEADER_SIZE], packet.payload.size() - HEADER_SIZE, 1, 1);
            if (ret != BZ_OK && ret != BZ_STREAM_END)
            {
                logger->error("Failed decomressing Bzip2 data! Error : " + std::to_string(ret));
                return;
            }

            std::string outputFileName = directory + "/" + std::to_string(packet.header.packet_sequence_count) + ".xml";
            logger->info("Writing message to " + outputFileName);
            std::ofstream outputMessageFile(outputFileName);
            outputMessageFile.write((char *)message_out, outsize);
            outputMessageFile.close();

            // TLE Parsing
            try
            {
                std::string xml_str(message_out, &message_out[outsize]);

                // Parse
                doc.parse<0>((char *)xml_str.c_str()); // const char * to char * is needed...
                root_node = doc.first_node("multi-mission-administrative-message");
                if (root_node == nullptr)
                {
                    logger->warn("Admin message has no multi-mission-administrative-message element");
                    count++;
                    return;
                }

                // Convert to our usual TLE Registry stuff
                // Fork: only <message> elements, each needing both attributes and both TLE lines
                for (rapidxml::xml_node<> *sat_node = root_node->first_node("message"); sat_node; sat_node = sat_node->next_sibling("message"))
                {
                    rapidxml::xml_attribute<> *number = sat_node->first_attribute("satellite-number");
                    rapidxml::xml_attribute<> *name = sat_node->first_attribute("satellite");
                    if (number == nullptr || name == nullptr)
                    {
                        logger->warn("Admin message entry without satellite name or number, skipping");
                        continue;
                    }

                    satdump::TLE tle;
                    try
                    {
                        tle.norad = std::stoi(number->value());
                    }
                    catch (std::exception &)
                    {
                        logger->warn("Admin message entry with an invalid satellite number, skipping");
                        continue;
                    }
                    tle.name = name->value();

                    int linecnt = 0;
                    for (rapidxml::xml_node<> *sat_tle_node = sat_node->first_node("navigation"); sat_tle_node; sat_tle_node = sat_tle_node->next_sibling())
                    {
                        for (rapidxml::xml_node<> *tle_node = sat_tle_node->first_node("two-line-elements"); tle_node; tle_node = tle_node->next_sibling())
                        {
                            for (rapidxml::xml_node<> *line1_node = tle_node->first_node("line-1"); line1_node; line1_node = line1_node->next_sibling())
                            {
                                if (linecnt++ == 0)
                                    tle.line1 = line1_node->value();
                                else
                                    tle.line2 = line1_node->value();
                            }
                        }
                    }

                    // Done, save it
                    if (tle.line1.empty() || tle.line2.empty())
                    {
                        logger->warn("Admin message entry for %s without both TLE lines, skipping", tle.name.c_str());
                        continue;
                    }
                    tles.push_back(tle);
                }
            }
            catch (std::exception &e)
            {
                logger->error("Error parsing Admin message : %s", e.what());
            }

            count++;
        }
    } // namespace admin_msg
} // namespace metopsg