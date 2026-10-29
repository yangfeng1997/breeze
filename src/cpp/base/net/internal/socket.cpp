#include "socket.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <string_view>

namespace breeze
{

void Fd::reset(int fd) noexcept
{
    // close 失败（EINTR / EIO）也无可挽回：Linux 上 fd 此时已经释放，重试反而可能关掉别人的 fd。
    if (fd_ >= 0)
        ::close(fd_);
    fd_ = fd;
}

namespace
{

socklen_t ToSockaddr(const Endpoint& ep, sockaddr_storage& ss)
{
    std::memset(&ss, 0, sizeof(ss));
    switch (ep.kind())
    {
        case Endpoint::Kind::Tcp:
            if (ep.IsIPv6())
            {
                auto* a = reinterpret_cast<sockaddr_in6*>(&ss);
                a->sin6_family = AF_INET6;
                a->sin6_port = htons(ep.port());
                inet_pton(AF_INET6, ep.address().c_str(), &a->sin6_addr); // Endpoint 已校验过
                return sizeof(sockaddr_in6);
            }
            else
            {
                auto* a = reinterpret_cast<sockaddr_in*>(&ss);
                a->sin_family = AF_INET;
                a->sin_port = htons(ep.port());
                inet_pton(AF_INET, ep.address().c_str(), &a->sin_addr);
                return sizeof(sockaddr_in);
            }
        case Endpoint::Kind::Unix:
        {
            auto* a = reinterpret_cast<sockaddr_un*>(&ss);
            a->sun_family = AF_UNIX;
            std::memcpy(a->sun_path, ep.address().data(), ep.address().size()); // ≤107 字节，memset 已补结尾 '\0'
            return static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + ep.address().size() + 1);
        }
        case Endpoint::Kind::UnixAbstract:
        {
            auto* a = reinterpret_cast<sockaddr_un*>(&ss);
            a->sun_family = AF_UNIX;
            std::memcpy(a->sun_path + 1, ep.address().data(), ep.address().size()); // sun_path[0] == '\0' 即抽象名
            return static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + ep.address().size());
        }
    }
    return 0;
}

// 地址族不认识、未命名的 UDS 对端、或不满足 Endpoint 约束的地址，都得到 nullopt。
std::optional<Endpoint> FromSockaddr(const sockaddr_storage& ss, socklen_t len)
{
    switch (ss.ss_family)
    {
        case AF_INET:
        {
            const auto* a = reinterpret_cast<const sockaddr_in*>(&ss);
            char buf[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &a->sin_addr, buf, sizeof(buf));
            return Endpoint::Tcp(buf, ntohs(a->sin_port));
        }
        case AF_INET6:
        {
            const auto* a = reinterpret_cast<const sockaddr_in6*>(&ss);
            char buf[INET6_ADDRSTRLEN];
            inet_ntop(AF_INET6, &a->sin6_addr, buf, sizeof(buf));
            return Endpoint::Tcp(buf, ntohs(a->sin6_port));
        }
        case AF_UNIX:
        {
            const auto* a = reinterpret_cast<const sockaddr_un*>(&ss);
            constexpr std::size_t kBase = offsetof(sockaddr_un, sun_path);
            if (len <= kBase)
                return std::nullopt; // 未命名
            const std::size_t n = len - kBase;
            if (a->sun_path[0] == '\0')
                return Endpoint::UnixAbstract(std::string_view(a->sun_path + 1, n - 1));
            return Endpoint::Unix(std::string_view(a->sun_path, strnlen(a->sun_path, n)));
        }
        default:
            return std::nullopt;
    }
}

int SetIntOpt(int fd, int level, int name, int value)
{
    return ::setsockopt(fd, level, name, &value, sizeof(value)) == 0 ? 0 : errno;
}

// 探测路径上的 UDS 文件是否有人在监听；没人监听（残留）就删掉，让随后的 bind 能成功。
int ClearStaleUnixSocket(const Endpoint& ep)
{
    struct stat st{};
    if (::lstat(ep.address().c_str(), &st) != 0)
        return errno == ENOENT ? 0 : errno;
    if (!S_ISSOCK(st.st_mode))
        return EADDRINUSE; // 不是 socket 的文件不动，更不 unlink

    Fd probe;
    if (const int err = sock::Create(ep, probe); err != 0)
        return err;
    sockaddr_storage ss;
    const socklen_t len = ToSockaddr(ep, ss);
    if (::connect(probe.get(), reinterpret_cast<const sockaddr*>(&ss), len) == 0)
        return EADDRINUSE; // 连上了：有人在监听
    switch (errno)
    {
        case EAGAIN: // 非阻塞 UDS connect 在对方 backlog 已满时返回它：同样说明有人在监听
        case EINPROGRESS:
            return EADDRINUSE;
        case ECONNREFUSED: // 文件还在但没人监听：上次进程没来得及清理的残留
            return (::unlink(ep.address().c_str()) == 0 || errno == ENOENT) ? 0 : errno;
        case ENOENT: // 探测期间被别人删掉了
            return 0;
        default:
            return errno;
    }
}

} // namespace

namespace sock
{

int Create(const Endpoint& ep, Fd& out)
{
    const int domain = ep.kind() == Endpoint::Kind::Tcp ? (ep.IsIPv6() ? AF_INET6 : AF_INET) : AF_UNIX;
    const int fd = ::socket(domain, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return errno;
    out.reset(fd);
    return 0;
}

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

int Bind(int fd, const Endpoint& ep)
{
    if (ep.kind() == Endpoint::Kind::Unix)
    {
        if (const int err = ClearStaleUnixSocket(ep); err != 0)
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

int Accept(int listen_fd, Fd& out, std::optional<Endpoint>& peer)
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

int SoError(int fd)
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

int GetPeerCredentials(int fd, PeerCredentials& out)
{
    ucred cred{};
    socklen_t len = sizeof(cred);
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0)
        return errno;
    out = PeerCredentials{cred.uid, cred.gid, cred.pid};
    return 0;
}

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

} // namespace sock

} // namespace breeze
