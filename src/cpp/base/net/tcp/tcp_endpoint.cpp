#include "base/net/tcp/tcp_endpoint.h"

namespace breeze
{

std::optional<TcpEndpoint> TcpEndpoint::Make(std::string_view ip, std::uint16_t port)
{
    if (auto ep = Endpoint::Tcp(ip, port))
        return TcpEndpoint(std::move(*ep));
    return std::nullopt;
}

std::optional<TcpEndpoint> TcpEndpoint::Parse(std::string_view text)
{
    if (const auto ep = Endpoint::Parse(text))
        return From(*ep);
    return std::nullopt;
}

std::optional<TcpEndpoint> TcpEndpoint::From(const Endpoint& ep)
{
    if (ep.kind() != Endpoint::Kind::Tcp)
        return std::nullopt;
    return TcpEndpoint(ep);
}

} // namespace breeze
