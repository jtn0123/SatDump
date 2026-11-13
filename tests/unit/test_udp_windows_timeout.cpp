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

// Fork: truncation reporting is opt-in; the old recv overload keeps its error behavior.
TEST_CASE("Windows UDP receive reports truncation without forwarding a prefix")
{
    fake_windows_net::UDPServer server(0);
    uint8_t buffer[17]{};
    bool truncated = false;
    fake_winsock::last_error = WSAEMSGSIZE;
    CHECK(server.recv(buffer, sizeof(buffer), &truncated) == sizeof(buffer));
    CHECK(truncated);
    CHECK_THROWS_AS(server.recv(buffer, sizeof(buffer)), std::runtime_error);
    fake_winsock::last_error = 10022;
}
TEST_CASE("Windows UDP receive preserves short packets and timeout retries")
{
    fake_windows_net::UDPServer server(0);
    uint8_t buffer[17]{};
    bool truncated = true;
    fake_winsock::receive_result = 15;
    CHECK(server.recv(buffer, sizeof(buffer), &truncated) == 15);
    CHECK_FALSE(truncated);
    fake_winsock::receive_result = -1;
    for (int error : {WSAETIMEDOUT, WSAEWOULDBLOCK, WSAEINTR})
    {
        fake_winsock::last_error = error;
        CHECK(server.recv(buffer, sizeof(buffer), &truncated) == 0);
        CHECK_FALSE(truncated);
    }
    fake_winsock::last_error = 10022;
}
