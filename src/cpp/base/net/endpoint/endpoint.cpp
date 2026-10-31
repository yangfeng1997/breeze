#include "endpoint.h"

#include <arpa/inet.h>
#include <sys/socket.h>

#include <array>
#include <utility>

namespace breeze
{

namespace
{

constexpr std::size_t kMaxUnixName = 107;

struct ParsedIp
{
    std::string text; // 规范化文本
    bool v6;
};

// 规范化 IP 字面量。inet_pton 吃 C 字符串，遇到内嵌 '\0' 会悄悄截断，所以先拒绝。
std::optional<ParsedIp> ParseIp(std::string_view ip)
{
    if (ip.empty() || ip.find('\0') != std::string_view::npos)
        return std::nullopt;
    const std::string tmp(ip);
    std::array<unsigned char, sizeof(in6_addr)> addr{};
    std::array<char, INET6_ADDRSTRLEN> out{};
    if (inet_pton(AF_INET, tmp.c_str(), addr.data()) == 1)
    {
        inet_ntop(AF_INET, addr.data(), out.data(), out.size());
        return ParsedIp{out.data(), false};
    }
    if (inet_pton(AF_INET6, tmp.c_str(), addr.data()) == 1)
    {
        inet_ntop(AF_INET6, addr.data(), out.data(), out.size());
        return ParsedIp{out.data(), true};
    }
    return std::nullopt;
}

std::optional<std::uint16_t> ParsePort(std::string_view s)
{
    if (s.empty() || s.size() > 5)
        return std::nullopt;
    unsigned v = 0;
    for (const char c : s)
    {
        if (c < '0' || c > '9')
            return std::nullopt;
        v = v * 10 + static_cast<unsigned>(c - '0');
    }
    if (v > 65535)
        return std::nullopt;
    return static_cast<std::uint16_t>(v);
}

bool ValidUnixName(std::string_view s)
{
    return !s.empty() && s.size() <= kMaxUnixName && s.find('\0') == std::string_view::npos;
}

bool ConsumePrefix(std::string_view& s, std::string_view prefix)
{
    if (!s.starts_with(prefix))
        return false;
    s.remove_prefix(prefix.size());
    return true;
}

} // namespace

std::optional<Endpoint> Endpoint::Tcp(std::string_view ip, std::uint16_t port)
{
    auto parsed = ParseIp(ip);
    if (!parsed)
        return std::nullopt;
    return Endpoint(Kind::Tcp, std::move(parsed->text), port);
}

std::optional<Endpoint> Endpoint::Udp(std::string_view ip, std::uint16_t port)
{
    auto parsed = ParseIp(ip);
    if (!parsed)
        return std::nullopt;
    return Endpoint(Kind::Udp, std::move(parsed->text), port);
}

std::optional<Endpoint> Endpoint::Unix(std::string_view path)
{
    if (!ValidUnixName(path) || path.front() != '/')
        return std::nullopt;
    return Endpoint(Kind::Unix, std::string(path), 0);
}

std::optional<Endpoint> Endpoint::UnixAbstract(std::string_view name)
{
    if (!ValidUnixName(name))
        return std::nullopt;
    return Endpoint(Kind::UnixAbstract, std::string(name), 0);
}

std::optional<Endpoint> Endpoint::Parse(std::string_view text)
{
    std::string_view rest = text;
    // tcp:// 与 udp:// 的主机与端口写法完全一样：下面共用一段解析，只有最后的种类不同。
    Kind ip_kind = Kind::Tcp;
    bool is_ip = false;
    if (ConsumePrefix(rest, "tcp://"))
        is_ip = true;
    else if (ConsumePrefix(rest, "udp://"))
    {
        is_ip = true;
        ip_kind = Kind::Udp;
    }
    if (is_ip)
    {
        std::string_view host;
        std::string_view port_text;
        bool want_v6 = false;
        if (!rest.empty() && rest.front() == '[')
        {
            const auto close = rest.find(']');
            if (close == std::string_view::npos || close + 1 >= rest.size() || rest[close + 1] != ':')
                return std::nullopt;
            host = rest.substr(1, close - 1);
            port_text = rest.substr(close + 2);
            want_v6 = true;
        }
        else
        {
            const auto colon = rest.rfind(':');
            if (colon == std::string_view::npos)
                return std::nullopt;
            host = rest.substr(0, colon);
            port_text = rest.substr(colon + 1);
        }
        const auto port = ParsePort(port_text);
        const auto ip = ParseIp(host);
        // 方括号里必须是 v6，没有方括号必须是 v4：避免 "::1:80" 这类有歧义的写法。
        if (!port || !ip || ip->v6 != want_v6)
            return std::nullopt;
        return Endpoint(ip_kind, ip->text, *port);
    }
    if (ConsumePrefix(rest, "unix-abstract://"))
        return UnixAbstract(rest);
    if (ConsumePrefix(rest, "unix://"))
        return Unix(rest);
    return std::nullopt;
}

std::string Endpoint::ToString() const
{
    switch (kind_)
    {
        case Kind::Tcp:
        case Kind::Udp:
        {
            const char* scheme = kind_ == Kind::Tcp ? "tcp://" : "udp://";
            return IsIPv6() ? scheme + ("[" + address_ + "]:" + std::to_string(port_))
                            : scheme + (address_ + ":" + std::to_string(port_));
        }
        case Kind::Unix:
            return "unix://" + address_;
        case Kind::UnixAbstract:
            return "unix-abstract://" + address_;
    }
    return {};
}

} // namespace breeze
