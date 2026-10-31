// 客户端的传输无关接口：StreamClient（TCP / UDS，经 TcpClient / UdsClient）与将来的 RUdpClient 都实现它。
// NetModule 经它持有各种客户端，业务代码经它发送；要类型化的端点与选项，直接用具体类。
// 设计见 docs/网络库设计.md §4.1 / §4.13。

#pragma once

#include <functional>

#include "connection_handle.h"
#include "endpoint.h"
#include "send_result.h"
#include "shared_buffer.h"

namespace breeze
{

// 每次连接尝试失败时回调（IO 线程，必须非阻塞、noexcept）：OnClosed 只在连接建立之后才有，连不上要靠它观察。
// will_retry 为 false 表示遇到不可重试的错误（字节流：EACCES / EPERM / EAFNOSUPPORT / EBADF / EFAULT / ENOTSOCK / EINVAL /
// ENAMETOOLONG；rudp：握手超时）：状态已回到 Idle，此后不再尝试，需要调用方显式 Start() 重来（回调里直接调 Start() 是合法的）。
// 放在各客户端的选项里而不是 ConnectionHandler：服务端也用后者。
using ConnectFailedCallback = std::move_only_function<void(int sys_errno, bool will_retry) noexcept>;

class NetClient
{
public:
    NetClient() = default;
    virtual ~NetClient() = default;

    NetClient(const NetClient&) = delete;
    NetClient& operator=(const NetClient&) = delete;

    // 开始连接并自动重连。任意线程；group 必须已 Start（否则是编程错误）。只返回参数类错误（选项不合法 EINVAL 等）；
    // 连接建立失败不在这里报，走各实现的失败回调。group 正在 / 已经停机返回 ESHUTDOWN。
    [[nodiscard]] virtual int Start() = 0;

    // 任意线程。停止重连并关闭当前连接，回到 Idle，不等待。该连接的 OnClosed 仍会且仅会触发一次。对 Idle 是空操作。
    virtual void Stop() = 0;

    // 经当前连接发送。未连通 → Closed，不缓冲。线程安全。
    [[nodiscard]] virtual SendResult Send(SharedBuffer payload) = 0;

    // 当前连接的句柄，没有连接时 raw() == 0。
    virtual ConnectionHandle handle() const noexcept = 0;

    // 对端地址（构造时给的）。
    virtual const Endpoint& endpoint() const noexcept = 0;
};

} // namespace breeze
