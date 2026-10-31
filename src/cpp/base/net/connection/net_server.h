// 服务端的传输无关接口：StreamServer（TCP / UDS，经 TcpServer / UdsServer）与将来的 RudpServer 都实现它。
// NetModule 经它持有各种服务端，业务代码需要"某个服务端的统一句柄"时也用它；要类型化的端点与选项，直接用具体类。
// 设计见 docs/网络库设计.md §4.1 / §4.13。

#pragma once

#include <functional>

#include "endpoint.h"
#include "peer_info.h"

namespace breeze
{

// 接受前过滤：在 accept 所在 IO 线程运行，返回 false 就立即拒绝这条连接（不产生任何回调，计入 rejected_filter）。
// UDS 的 PeerInfo 带 credentials（uid / gid / pid），可据此校验对端身份。必须非阻塞、noexcept。
// 放在各服务端的选项里而不是 ConnectionHandler：客户端也用后者。
using AcceptFilter = std::move_only_function<bool(const PeerInfo&) noexcept>;

class NetServer
{
public:
    NetServer() = default;
    virtual ~NetServer() = default;

    NetServer(const NetServer&) = delete;
    NetServer& operator=(const NetServer&) = delete;

    // 开始接受连接。group 必须已 Start（否则是编程错误）；只能调用一次。0 = 成功，否则 errno，失败时已回滚；
    // group 正在 / 已经停机返回 ESHUTDOWN。具体的错误码见各实现（比如 EADDRINUSE、EPROTONOSUPPORT）。
    [[nodiscard]] virtual int Start() = 0;

    // 只停止接受新连接，已建立的连接继续服务，由 EventLoopGroup::Shutdown 收尾。异步；对没 Start 过的对象是空操作。
    virtual void Stop() = 0;

    // 实际绑定的地址：端口填 0 时 Start 成功之后这里是系统分配的端口。Start 之前就是构造时给的端点。
    virtual const Endpoint& endpoint() const noexcept = 0;
};

} // namespace breeze
