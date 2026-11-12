#include "webserver.h"
#include "logger.h"
#include <vector>
#include <mutex>

// Webserver for stats
namespace webserver
{
    nng_http_server *http_server;
    nng_url *url;
    nng_http_handler *handler;
    nng_http_handler *handler_html;
    nng_http_handler *handler_polarplot;
    nng_http_handler *handler_fft;
    nng_http_handler *handler_schedule;

    bool is_active = false;

    std::mutex request_mutex;

    std::function<std::string()> handle_callback = []() -> std::string
    { return ""; };

    std::function<std::string(std::string)> handle_callback_html = [](std::string) -> std::string
    { return "Please use /api for JSON data"; };

    std::function<std::vector<uint8_t>()> handle_callback_polarplot = []() -> std::vector<uint8_t>
    { return {}; };

    std::function<std::vector<uint8_t>()> handle_callback_fft = []() -> std::vector<uint8_t>
    { return {}; };

    std::function<std::vector<uint8_t>()> handle_callback_schedule = []() -> std::vector<uint8_t>
    { return {}; };

    // HTTP Handler for stats
    void http_handle(nng_aio *aio)
    {
        request_mutex.lock();

        std::string jsonstr = handle_callback();

        nng_http_res *res;
        nng_http_res_alloc(&res);
        nng_http_res_copy_data(res, jsonstr.c_str(), jsonstr.size());
        nng_http_res_set_header(res, "Content-Type", "application/json; charset=utf-8");
        nng_aio_set_output(aio, 0, res);
        nng_aio_finish(aio, 0);
        request_mutex.unlock();
    }

    // HTTP Handler for HTML
    void http_handle_html(nng_aio *aio)
    {
        request_mutex.lock();

        std::string uri = nng_http_req_get_uri((nng_http_req *)nng_aio_get_input(aio, 0));

        std::string jsonstr = handle_callback_html(uri);

        nng_http_res *res;
        nng_http_res_alloc(&res);
        nng_http_res_copy_data(res, jsonstr.c_str(), jsonstr.size());
        nng_http_res_set_header(res, "Content-Type", "text/html; charset=utf-8");
        nng_aio_set_output(aio, 0, res);
        nng_aio_finish(aio, 0);
        request_mutex.unlock();
    }

    bool add_polarplot_handler = false;

    // HTTP Handler for HTML
    void http_handle_polarplot(nng_aio *aio)
    {
        request_mutex.lock();

        std::vector<uint8_t> img = handle_callback_polarplot();

        nng_http_res *res;
        nng_http_res_alloc(&res);
        nng_http_res_copy_data(res, img.data(), img.size());
        nng_http_res_set_header(res, "Content-Type", "image/jpeg");
        nng_aio_set_output(aio, 0, res);
        nng_aio_finish(aio, 0);
        request_mutex.unlock();
    }

    // HTTP Handler for HTML
    void http_handle_fft(nng_aio *aio)
    {
        request_mutex.lock();

        std::vector<uint8_t> img = handle_callback_fft();

        nng_http_res *res;
        nng_http_res_alloc(&res);
        nng_http_res_copy_data(res, img.data(), img.size());
        nng_http_res_set_header(res, "Content-Type", "image/jpeg");
        nng_aio_set_output(aio, 0, res);
        nng_aio_finish(aio, 0);
        request_mutex.unlock();
    }

    // HTTP Handler for Schedule
    void http_handle_schedule(nng_aio *aio)
    {
        request_mutex.lock();

        std::vector<uint8_t> img = handle_callback_schedule();

        nng_http_res *res;
        nng_http_res_alloc(&res);
        nng_http_res_copy_data(res, img.data(), img.size());
        nng_http_res_set_header(res, "Content-Type", "image/jpeg");
        nng_aio_set_output(aio, 0, res);
        nng_aio_finish(aio, 0);
        request_mutex.unlock();
    }

    // Fork: adds one handler, and frees it if the server does not take it
    static int add_handler(nng_http_handler **h, const char *path, void (*fn)(nng_aio *), bool tree)
    {
        int rv = nng_http_handler_alloc(h, path, fn);
        if (rv != 0)
            return rv;
        if ((rv = nng_http_handler_set_method(*h, "GET")) == 0 && (!tree || (rv = nng_http_handler_set_tree(*h)) == 0) && (rv = nng_http_server_add_handler(http_server, *h)) == 0)
            return 0;
        nng_http_handler_free(*h);
        return rv;
    }

    bool start(std::string http_server_url)
    {
        // Fork: every step is checked. A server that cannot start (another program on the port,
        // an address that is not host:port) is reported and start() returns false; the errors
        // were ignored, so the run went on with nothing answering on the address
        int rv = nng_url_parse(&url, ("http://" + http_server_url).c_str());
        if (rv != 0)
        {
            logger->error("Cannot start the web server: %s is not an address and port such as 127.0.0.1:8080 (%s)", http_server_url.c_str(), nng_strerror(rv));
            return false;
        }

        if ((rv = nng_http_server_hold(&http_server, url)) != 0)
        {
            nng_url_free(url);
            logger->error("Cannot start the web server on %s: %s", http_server_url.c_str(), nng_strerror(rv));
            return false;
        }

        const char *step = "/api";
        rv = add_handler(&handler, "/api", http_handle, false);
        if (rv == 0)
        {
            step = "pages";
            rv = add_handler(&handler_html, "", http_handle_html, true);
        }

        if (rv == 0 && add_polarplot_handler)
        {
            step = "/polarplot.jpeg";
            rv = add_handler(&handler_polarplot, "/polarplot.jpeg", http_handle_polarplot, false);
            if (rv == 0)
            {
                step = "/fft.jpeg";
                rv = add_handler(&handler_fft, "/fft.jpeg", http_handle_fft, false);
            }
            if (rv == 0)
            {
                step = "/schedule.jpeg";
                rv = add_handler(&handler_schedule, "/schedule.jpeg", http_handle_schedule, false);
            }
        }

        if (rv == 0)
        {
            step = nullptr;
            rv = nng_http_server_start(http_server);
        }

        nng_url_free(url);
        if (rv != 0)
        {
            nng_http_server_release(http_server);
            if (step != nullptr)
                logger->error("Cannot start the web server on %s: %s could not be set up (%s)", http_server_url.c_str(), step, nng_strerror(rv));
            else if (rv == NNG_EADDRINUSE)
                logger->error("Cannot start the web server on %s: the address is in use (another program, or another SatDump, uses this port)", http_server_url.c_str());
            else
                logger->error("Cannot start the web server on %s: %s", http_server_url.c_str(), nng_strerror(rv));
            return false;
        }

        is_active = true;
        return true;
    }

    void stop()
    {
        if (is_active)
        {
            nng_http_server_stop(http_server);
            nng_http_server_release(http_server);
            is_active = false; // Fork: so a second stop() does nothing
        }
    }
};
