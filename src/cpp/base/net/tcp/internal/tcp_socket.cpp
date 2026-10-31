#include "tcp_socket.h"

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include <cerrno>
#include <cstring>

namespace breeze
{

namespace
{

int SetIntOpt(int fd, int level, int name, int value)
{
    return ::setsockopt(fd, level, name, &value, sizeof(value)) == 0 ? 0 : errno;
}

} // namespace

namespace tcp
{

int SetNoDelay(int fd) { return SetIntOpt(fd, IPPROTO_TCP, TCP_NODELAY, 1); }

int SetReuseAddr(int fd) { return SetIntOpt(fd, SOL_SOCKET, SO_REUSEADDR, 1); }

int SetKeepAlive(int fd, const KeepAlive& ka)
{
    if (const int e = SetIntOpt(fd, SOL_SOCKET, SO_KEEPALIVE, 1); e != 0)
        return e;
    if (const int e = SetIntOpt(fd, IPPROTO_TCP, TCP_KEEPIDLE, ka.idle_s); e != 0)
        return e;
    if (const int e = SetIntOpt(fd, IPPROTO_TCP, TCP_KEEPINTVL, ka.interval_s); e != 0)
        return e;
    if (const int e = SetIntOpt(fd, IPPROTO_TCP, TCP_KEEPCNT, ka.count); e != 0)
        return e;
    if (ka.user_timeout_ms > 0)
        return SetIntOpt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, ka.user_timeout_ms);
    return 0;
}

int SetUserTimeout(int fd, int ms) { return SetIntOpt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, ms); }

bool IsSelfConnect(int fd)
{
    sockaddr_storage a{};
    sockaddr_storage b{};
    socklen_t la = sizeof(a);
    socklen_t lb = sizeof(b);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &la) != 0 ||
        ::getpeername(fd, reinterpret_cast<sockaddr*>(&b), &lb) != 0 || a.ss_family != b.ss_family)
        return false;
    if (a.ss_family == AF_INET)
    {
        const auto* x = reinterpret_cast<const sockaddr_in*>(&a);
        const auto* y = reinterpret_cast<const sockaddr_in*>(&b);
        return x->sin_port == y->sin_port && x->sin_addr.s_addr == y->sin_addr.s_addr;
    }
    if (a.ss_family == AF_INET6)
    {
        const auto* x = reinterpret_cast<const sockaddr_in6*>(&a);
        const auto* y = reinterpret_cast<const sockaddr_in6*>(&b);
        return x->sin6_port == y->sin6_port && std::memcmp(&x->sin6_addr, &y->sin6_addr, sizeof(in6_addr)) == 0;
    }
    return false;
}

} // namespace tcp

} // namespace breeze
