#include "uds_endpoint.h"

namespace breeze
{

std::optional<UdsEndpoint> UdsEndpoint::Path(std::string_view path)
{
    if (auto ep = Endpoint::Unix(path))
        return UdsEndpoint(std::move(*ep));
    return std::nullopt;
}

std::optional<UdsEndpoint> UdsEndpoint::Abstract(std::string_view name)
{
    if (auto ep = Endpoint::UnixAbstract(name))
        return UdsEndpoint(std::move(*ep));
    return std::nullopt;
}

std::optional<UdsEndpoint> UdsEndpoint::Parse(std::string_view text)
{
    if (const auto ep = Endpoint::Parse(text))
        return From(*ep);
    return std::nullopt;
}

std::optional<UdsEndpoint> UdsEndpoint::From(const Endpoint& ep)
{
    if (ep.kind() != Endpoint::Kind::Unix && ep.kind() != Endpoint::Kind::UnixAbstract)
        return std::nullopt;
    return UdsEndpoint(ep);
}

} // namespace breeze
