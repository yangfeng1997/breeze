#include "base/net/rudp/rudp_endpoint.h"

namespace breeze
{

std::optional<RUdpEndpoint> RUdpEndpoint::Make(std::string_view ip, std::uint16_t port)
{
    if (auto ep = Endpoint::Udp(ip, port))
        return RUdpEndpoint(std::move(*ep));
    return std::nullopt;
}

std::optional<RUdpEndpoint> RUdpEndpoint::Parse(std::string_view text)
{
    if (const auto ep = Endpoint::Parse(text))
        return From(*ep);
    return std::nullopt;
}

std::optional<RUdpEndpoint> RUdpEndpoint::From(const Endpoint& ep)
{
    if (ep.kind() != Endpoint::Kind::Udp)
        return std::nullopt;
    return RUdpEndpoint(ep);
}

} // namespace breeze
