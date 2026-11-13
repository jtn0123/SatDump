// Fork: force a real POSIX helper's timeout setup to fail, without contacting a host.
#include "doctest.h"
#include <stdexcept>
#include <system_error>
#include <cerrno>
#include <cstring>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/time.h>
namespace
{
    bool reject_timeout = false;
    int checked_setsockopt(int fd, int level, int option, const void *value, socklen_t size)
    {
        if (option == SO_RCVTIMEO && reject_timeout)
        {
            errno = EINVAL;
            return -1;
        }
        return ::setsockopt(fd, level, option, value, size);
    }
}
#define setsockopt checked_setsockopt
#define net checked_posix_net
#include "common/net/udp.h"
#undef net
#undef setsockopt

TEST_CASE("POSIX UDP timeout setup errors are reported")
{
    checked_posix_net::UDPServer server(0);
    CHECK_NOTHROW(server.enableTimeout());
    reject_timeout = true;
    CHECK_THROWS_AS(server.enableTimeout(), std::system_error);
    reject_timeout = false;
}
