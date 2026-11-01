#include "base/net/agent/route_table.h"

#include <algorithm>

namespace breeze::agent
{

void StaticRouteTable::SetInstances(std::uint16_t service_type, std::vector<NodeId> nodes)
{
    std::sort(nodes.begin(), nodes.end());
    nodes.erase(std::unique(nodes.begin(), nodes.end()), nodes.end());
    Service& s = services_[service_type];
    s.nodes = std::move(nodes);
    s.next = 0;
}

void StaticRouteTable::Resolve(const EnvelopeHead& head, std::vector<NodeId>& out)
{
    switch (static_cast<RouteType>(head.route_type))
    {
        case RouteType::kDirect:
            out.push_back(head.dst);
            return;
        case RouteType::kAny:
        {
            const auto it = services_.find(head.service_type);
            if (it == services_.end() || it->second.nodes.empty())
                return;
            Service& s = it->second;
            out.push_back(s.nodes[s.next++ % s.nodes.size()]);
            return;
        }
        case RouteType::kHash:
        {
            const auto it = services_.find(head.service_type);
            if (it == services_.end() || it->second.nodes.empty())
                return;
            const auto& nodes = it->second.nodes;
            out.push_back(nodes[head.route_key % nodes.size()]);
            return;
        }
        case RouteType::kBroadcast:
        {
            const auto it = services_.find(head.service_type);
            if (it == services_.end())
                return;
            for (const NodeId n : it->second.nodes)
            {
                if (n != head.src)
                    out.push_back(n);
            }
            return;
        }
    }
}

ConnectionHandle StaticRouteTable::RemoteOf(NodeId node) const
{
    const auto it = remote_.find(node);
    return it == remote_.end() ? ConnectionHandle{} : it->second;
}

} // namespace breeze::agent
