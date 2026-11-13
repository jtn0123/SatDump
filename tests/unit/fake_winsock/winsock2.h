#pragma once
// Fork: a Winsock contract double; no sockets or network traffic are created.
#include <cstdint>
#include <cstring>

using DWORD = std::uint32_t;
struct WSADATA {};
struct in_addr
{
    union { std::uint32_t s_addr; struct { std::uint32_t S_addr; } S_un; };
};
struct sockaddr { unsigned short family; char data[14]; };
struct sockaddr_in { short sin_family; unsigned short sin_port; in_addr sin_addr; char padding[8]; };
constexpr int AF_INET = 2, SOCK_DGRAM = 2, IPPROTO_UDP = 17;
constexpr int SOL_SOCKET = 65535, SO_REUSEADDR = 4, SO_RCVTIMEO = 4102;
constexpr int INADDR_ANY = 0, WSAEINTR = 10004;
// Fork: receive-path results used by the real UDP helper on Windows.
constexpr int WSAEWOULDBLOCK = 10035, WSAEMSGSIZE = 10040, WSAETIMEDOUT = 10060;
// The old helper uses timeval even on Windows; retain the type to test that regression.
#ifndef __timeval_defined
struct timeval { long tv_sec; long tv_usec; };
#endif
namespace fake_winsock
{
    inline int timeout_bytes = 0;
    inline DWORD timeout_ms = 0;
    inline bool reject_timeout = false;
    inline int last_error = 10022, receive_result = -1;
}
inline int MAKEWORD(int, int) { return 0; }
inline int WSAStartup(int, WSADATA *) { return 0; }
inline int WSACleanup() { return 0; }
inline int WSAGetLastError() { return fake_winsock::last_error; }
inline int socket(int, int, int) { return 7; }
inline int closesocket(int) { return 0; }
inline int bind(int, const sockaddr *, int) { return 0; }
inline unsigned short htons(unsigned short value) { return value; }
inline std::uint32_t inet_addr(const char *) { return 0; }
inline int setsockopt(int, int, int option, const char *value, int bytes)
{
    if (option != SO_RCVTIMEO) return 0;
    fake_winsock::timeout_bytes = bytes;
    if (bytes == sizeof(DWORD))
        std::memcpy(&fake_winsock::timeout_ms, value, sizeof(DWORD));
    return fake_winsock::reject_timeout || bytes != sizeof(DWORD) ? -1 : 0;
}
inline int sendto(int, const char *, int size, int, const sockaddr *, int) { return size; }
inline int recvfrom(int, char *, int, int, sockaddr *, int *) { return fake_winsock::receive_result; }
