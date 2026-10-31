#include "rudp_endpoint.h"

namespace breeze
{

std::optional<RudpEndpoint> RudpEndpoint::Make(std::string_view ip, std::uint16_t port)
{
    if (auto ep = Endpoint::Udp(ip, port))
        return RudpEndpoint(std::move(*ep));
    return std::nullopt;
}

std::optional<RudpEndpoint> RudpEndpoint::Parse(std::string_view text)
{
    if (const auto ep = Endpoint::Parse(text))
        return From(*ep);
    return std::nullopt;
}

std::optional<RudpEndpoint> RudpEndpoint::From(const Endpoint& ep)
{
    if (ep.kind() != Endpoint::Kind::Udp)
        return std::nullopt;
    return RudpEndpoint(ep);
}

} // namespace breeze
