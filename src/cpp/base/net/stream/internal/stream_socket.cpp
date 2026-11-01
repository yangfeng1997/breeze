#include "base/net/stream/internal/stream_socket.h"

#include <sys/socket.h>

#include <cerrno>

#include "base/net/socket/socket_address.h"
#include "base/net/uds/uds_socket.h"

namespace breeze
{

namespace sock
{

int CreateStreamSocket(const Endpoint& ep, UniqueFd& out) { return CreateSocket(ep, SOCK_STREAM, out); }

int Bind(int fd, const Endpoint& ep)
{
    if (ep.kind() == Endpoint::Kind::Unix)
    {
        if (const int err = uds::ClearStaleSocket(ep); err != 0)
            return err;
    }
    sockaddr_storage ss;
    const socklen_t len = ToSockaddr(ep, ss);
    return ::bind(fd, reinterpret_cast<const sockaddr*>(&ss), len) == 0 ? 0 : errno;
}

int Listen(int fd, int backlog) { return ::listen(fd, backlog) == 0 ? 0 : errno; }

int Connect(int fd, const Endpoint& ep)
{
    sockaddr_storage ss;
    const socklen_t len = ToSockaddr(ep, ss);
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&ss), len) == 0)
        return 0;
    // 被信号打断的非阻塞 connect 会在后台继续建立，与 EINPROGRESS 等价。
    return errno == EINTR ? EINPROGRESS : errno;
}

int Accept(int listen_fd, UniqueFd& out, std::optional<Endpoint>& peer)
{
    sockaddr_storage ss;
    socklen_t len;
    int fd;
    do
    {
        len = sizeof(ss);
        fd = ::accept4(listen_fd, reinterpret_cast<sockaddr*>(&ss), &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0)
        return errno;
    out.reset(fd);
    peer = FromSockaddr(ss, len);
    return 0;
}

int GetSocketError(int fd)
{
    int err = 0;
    socklen_t len = sizeof(err);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0)
        return errno;
    return err;
}

int LocalEndpoint(int fd, std::optional<Endpoint>& out)
{
    sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&ss), &len) != 0)
        return errno;
    out = FromSockaddr(ss, len);
    return 0;
}

int PeerEndpoint(int fd, std::optional<Endpoint>& out)
{
    sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    if (::getpeername(fd, reinterpret_cast<sockaddr*>(&ss), &len) != 0)
        return errno;
    out = FromSockaddr(ss, len);
    return 0;
}

} // namespace sock

} // namespace breeze
