#include "doppler_qth.h"
#include "logger.h"
#include <exception>

namespace satdump
{
    namespace pipeline
    {
        namespace demod
        {
            DopplerQTH resolve_doppler_qth(const nlohmann::ordered_json &general_settings, const nlohmann::json &parameters)
            {
                DopplerQTH qth;
                const char *keys[3] = {"qth_lon", "qth_lat", "qth_alt"};
                double *values[3] = {&qth.lon, &qth.lat, &qth.alt};

                for (int i = 0; i < 3; i++)
                {
                    // A pipeline parameter replaces the setting (an unreadable one stops the module, as before)
                    if (parameters.contains(keys[i]))
                    {
                        *values[i] = parameters[keys[i]].get<double>();
                        continue;
                    }

                    try
                    {
                        *values[i] = general_settings.at(keys[i]).at("value").get<double>();
                    }
                    catch (std::exception &e)
                    {
                        logger->warn("Doppler correction: %s could not be read from the settings (%s), using 0", keys[i], e.what());
                    }
                }

                if (qth.lon == 0 && qth.lat == 0)
                    logger->warn("Doppler correction: the station position is 0/0 (not set in the settings or the pipeline parameters), "
                                 "so the correction is computed for that place. Set qth_lat and qth_lon in the settings.");

                return qth;
            }
        } // namespace demod
    } // namespace pipeline
} // namespace satdump
