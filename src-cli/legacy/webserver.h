#pragma once

#include "nlohmann/json.hpp"
#include <nng/nng.h>
#include <nng/supplemental/http/http.h>
#include <nng/supplemental/util/platform.h>
#include <functional>
#include <string>

// Webserver for stats
namespace webserver
{
    extern std::function<std::string()> handle_callback;
    extern std::function<std::string(std::string)> handle_callback_html;
    extern std::function<std::vector<uint8_t>()> handle_callback_polarplot;
    extern std::function<std::vector<uint8_t>()> handle_callback_fft;
    extern std::function<std::vector<uint8_t>()> handle_callback_schedule;

    extern bool add_polarplot_handler;

    // Fork: false when the server could not start, with the reason logged as an error (most
    // often another program already uses the port). It used to carry on with no server
    bool start(std::string http_server_url);

    // Fork: start() on the value of the --http_server flag. A value that is not text (a number,
    // or true for a flag given no value) is not an address either: it is reported the same way
    // and false is returned. Every line that reports a server that cannot start begins with
    // "Cannot start the web server on <value>:"
    bool start_from_flag(const nlohmann::json &http_server);

    void stop();
};