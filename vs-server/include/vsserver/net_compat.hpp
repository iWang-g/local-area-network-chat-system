#pragma once

/// Minimal Winsock vs POSIX socket wrappers for tcp_server.

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <WinSock2.h>
#include <WS2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <cstdint>

namespace vsserver {

#ifdef _WIN32
using SockHandle = SOCKET;
inline bool sockIsInvalid(SockHandle s) noexcept
{
    return s == INVALID_SOCKET;
}
inline int sockClose(SockHandle s) noexcept
{
    return ::closesocket(s);
}
inline bool recvWouldStop(int n) noexcept
{
    return n == SOCKET_ERROR || n == 0;
}
inline bool sendFailed(int n) noexcept
{
    return n == SOCKET_ERROR || n == 0;
}
inline bool sockStartup()
{
    WSADATA wsa{};
    return ::WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
}
inline void sockCleanup()
{
    ::WSACleanup();
}
#else
using SockHandle = int;
inline bool sockIsInvalid(SockHandle s) noexcept
{
    return s < 0;
}
inline int sockClose(SockHandle s) noexcept
{
    return ::close(s);
}
inline bool recvWouldStop(int n) noexcept
{
    return n <= 0;
}
inline bool sendFailed(int n) noexcept
{
    return n <= 0;
}
inline bool sockStartup()
{
    return true;
}
inline void sockCleanup()
{
}
#endif

} // namespace vsserver
