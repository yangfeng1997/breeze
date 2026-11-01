// Agent 的路由表：把一条消息信封里的目的（route_type / dst / service_type / route_key）展开成要投递的 Node 列表，
// 并回答"这个 Node 在别的 Agent 上时，经哪条连接发过去"。路由表的真正来源是控制面（服务发现），仓库里还没有；
// 这里只给接口，加一个内存实现（StaticRouteTable）供测试和简单部署用。
//
// 线程：AgentRouter 跑在一个 EventLoop 线程上（见 agent_router.h），路由表只在那个线程上被读；
// 要更新（控制面推来新的实例列表）就 Post 到那个 loop 上改，StaticRouteTable 自己不加锁。

#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "base/net/agent/envelope.h"
#include "base/net/connection/connection_handle.h"

namespace breeze::agent
{

class RouteTable
{
public:
    virtual ~RouteTable() = default;

    // 把消息的目的展开成要投递的 Node，追加到 out（调用方已清空）。一个都没有 = 无路可走。
    //   Direct     {dst}
    //   Any        service_type 的实例里选一个
    //   Hash       service_type 的实例里按 route_key 选一个
    //   Broadcast  service_type 的全部实例（不含发送方自己）
    virtual void Resolve(const EnvelopeHead& head, std::vector<NodeId>& out) = 0;

    // node 在别的 Agent 上：返回通向那个 Agent 的连接句柄；本机的 Node（或不认识的）返回无效句柄（raw() == 0）。
    virtual ConnectionHandle RemoteOf(NodeId node) const = 0;
};

// 内存实现。Any 轮询；Hash 对"按 NodeId 排序的实例列表"取模 —— **不是一致性哈希**：实例增减时大部分键会换去别的实例，
// 要保持粘性得由控制面给出带版本的映射（设计 §13）。
class StaticRouteTable final : public RouteTable
{
public:
    // 设置某个服务类型的全部实例（覆盖原有的）。内部排序、去重。
    void SetInstances(std::uint16_t service_type, std::vector<NodeId> nodes);
    // 登记 / 撤销"这个 Node 在远端 Agent 上"。
    void SetRemote(NodeId node, ConnectionHandle link) { remote_[node] = link; }
    void ClearRemote(NodeId node) { remote_.erase(node); }

    void Resolve(const EnvelopeHead& head, std::vector<NodeId>& out) override;
    ConnectionHandle RemoteOf(NodeId node) const override;

private:
    struct Service
    {
        std::vector<NodeId> nodes; // 升序
        std::size_t next = 0;      // Any 的轮询位置
    };
    std::unordered_map<std::uint16_t, Service> services_;
    std::unordered_map<NodeId, ConnectionHandle> remote_;
};

} // namespace breeze::agent
