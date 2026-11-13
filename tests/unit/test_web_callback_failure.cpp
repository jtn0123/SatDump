// Fork: C callbacks must finish their aio and release their request lock after any exception.
#include "../../src-cli/legacy/webserver.h"
#include "doctest.h"
#include <stdexcept>
namespace webserver { void http_handle(nng_aio *); }
TEST_CASE("web callback failure releases request lock and completes aio")
{
    nng_aio *aio = nullptr;
    REQUIRE(nng_aio_alloc(&aio, nullptr, nullptr) == 0);
    for (int kind : {0, 1})
    {
        webserver::handle_callback = [kind]() -> std::string
        {
            if (kind) throw 7;
            throw std::runtime_error("callback failure");
        };
        REQUIRE(nng_aio_begin(aio));
        CHECK_NOTHROW(webserver::http_handle(aio));
        nng_aio_wait(aio);
        CHECK(nng_aio_result(aio) == NNG_EINTERNAL);
        webserver::handle_callback = [] { return "{}"; };
        REQUIRE(nng_aio_begin(aio));
        CHECK_NOTHROW(webserver::http_handle(aio));
        nng_aio_wait(aio);
        CHECK(nng_aio_result(aio) == 0);
        auto *response = static_cast<nng_http_res *>(nng_aio_get_output(aio, 0));
        REQUIRE(response != nullptr);
        nng_http_res_free(response);
    }
    nng_aio_free(aio);
}
