#pragma once

/**
 * @file doppler_qth.h
 * @brief The station position used by Doppler correction in the demodulators
 */

#include "nlohmann/json.hpp"

namespace satdump
{
    namespace pipeline
    {
        namespace demod
        {
            /**
             * @brief A station position: longitude and latitude in degrees, altitude
             */
            struct DopplerQTH
            {
                double lon = 0;
                double lat = 0;
                double alt = 0;
            };

            /**
             * @brief Fork: the station position for Doppler correction, with a warning when
             * it is probably wrong (this used to be silent).
             *
             * qth_lon, qth_lat and qth_alt are taken from the pipeline parameters when given
             * there, else from the general settings ("satdump_general", whose entries hold a
             * "value"). A settings value that cannot be read is 0, with a warning naming it.
             * A position that is still 0/0 (the default when no station is set) gets a
             * warning too, since the correction is then computed for the wrong place.
             * @param general_settings the "satdump_general" part of the settings
             * @param parameters the module's pipeline parameters
             * @return the position to use
             */
            DopplerQTH resolve_doppler_qth(const nlohmann::ordered_json &general_settings, const nlohmann::json &parameters);
        } // namespace demod
    } // namespace pipeline
} // namespace satdump
