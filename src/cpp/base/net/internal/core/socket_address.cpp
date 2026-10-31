#include "socket_address.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/un.h>

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <string_view>

namespace breeze
{

namespace sock
{

socklen_t ToSockaddr(const Endpoint& ep, sockaddr_storage& ss)
{
    std::memset(&ss, 0, sizeof(ss));
    switch (ep.kind())
    {
        case Endpoint::Kind::Tcp:
        case Endpoint::Kind::Udp:
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

namespace
{

// IP 类地址按调用方要的种类（Tcp / Udp）构造；字节流的 accept / getsockname 要 Tcp，数据报的 recvfrom 要 Udp。
std::optional<Endpoint> MakeIp(Endpoint::Kind ip_kind, const char* ip, std::uint16_t port)
{
    return ip_kind == Endpoint::Kind::Udp ? Endpoint::Udp(ip, port) : Endpoint::Tcp(ip, port);
}

} // namespace

// 地址族不认识、未命名的 UDS 对端、或不满足 Endpoint 约束的地址，都得到 nullopt。
std::optional<Endpoint> FromSockaddr(const sockaddr_storage& ss, socklen_t len, Endpoint::Kind ip_kind)
{
    switch (ss.ss_family)
    {
        case AF_INET:
        {
            const auto* a = reinterpret_cast<const sockaddr_in*>(&ss);
            char buf[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &a->sin_addr, buf, sizeof(buf));
            return MakeIp(ip_kind, buf, ntohs(a->sin_port));
        }
        case AF_INET6:
        {
            const auto* a = reinterpret_cast<const sockaddr_in6*>(&ss);
            char buf[INET6_ADDRSTRLEN];
            inet_ntop(AF_INET6, &a->sin6_addr, buf, sizeof(buf));
            return MakeIp(ip_kind, buf, ntohs(a->sin6_port));
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

int CreateSocket(const Endpoint& ep, int type, UniqueFd& out)
{
    const int domain = ep.IsIp() ? (ep.IsIPv6() ? AF_INET6 : AF_INET) : AF_UNIX;
    const int fd = ::socket(domain, type | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return errno;
    out.reset(fd);
    return 0;
}

} // namespace sock

} // namespace breeze
