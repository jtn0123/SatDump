// Fork: compile the actual Windows helper branch against a Winsock contract double on Linux.
#include "doctest.h"
#include <cerrno>
#include <stdexcept>
#include <system_error>
#include <cstring>
// Keep the contract double separate from the real POSIX class in the core DSO.
#define net fake_windows_net
#define _WIN32
#include "common/net/udp.h"
#undef _WIN32
#undef net

TEST_CASE("Windows UDP timeout is a DWORD in milliseconds")
{
    fake_winsock::reject_timeout = false;
    fake_winsock::timeout_bytes = 0;
    fake_winsock::timeout_ms = 0;
    fake_windows_net::UDPServer server(0);
    CHECK_NOTHROW(server.enableTimeout());
    CHECK(fake_winsock::timeout_bytes == sizeof(DWORD));
    CHECK(fake_winsock::timeout_ms == 10);
}
TEST_CASE("Windows UDP timeout setup errors are reported")
{
    fake_winsock::reject_timeout = true;
    fake_windows_net::UDPServer server(0);
    CHECK_THROWS_AS(server.enableTimeout(), std::system_error);
    fake_winsock::reject_timeout = false;
}
