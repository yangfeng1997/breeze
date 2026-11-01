// rudp 客户端：凭准入票据向一个 RUdpServer 握手，得到一条可靠有序的消息通道。
// 设计与威胁模型见 docs/网络库设计.md §4.14。
//
// 与 TcpClient 的区别：
// - **不自动重连**：票据一次有效，断线后要从控制面拿新票据（SetTicket）再 Start()。
//   握手失败（拒绝 / 超时）与连接断开后都回到 Idle，业务据此决定重新申请票据的时机。
// - 没有 codec：消息自带边界。
// - 握手期间每 hello_interval 重发一次 Hello（UDP 会丢），到 connect_timeout 仍没结果就失败（ETIMEDOUT，
//   或期间收到的最后一个 socket 错误，比如服务端没起时的 ECONNREFUSED）。握手期间的 ECONNREFUSED 不算失败：
//   服务端可能正在起来，重试到超时为止。
// - 连通之后每 ping_interval 发一个心跳（服务端回应），所以两端的 idle_timeout 都可以放心打开；
//   ICMP 错误不关会话（偶发的不可达不该断线），断线靠服务端的 Close、idle_timeout 与 KCP 链路判死。
//
// 一个对象一个服务端，固定在第一次 Start() 时选定的一个 loop 上，自己不建线程。生命周期同 StreamClient：
// handler 只持引用，须比 EventLoopGroup::Shutdown 活得久；对象自己也须覆盖到 Shutdown 返回之后；
// 析构时若 group 仍在运行，会先同步停掉并等连接回收（保险，不是用法）。

#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>

#include "base/buffer/shared_buffer.h"
#include "base/net/connection/connection_handle.h"
#include "base/net/connection/connection_handler_interface.h"
#include "base/net/connection/connection_options.h"
#include "base/net/connection/net_client_interface.h"
#include "base/net/connection/send_result.h"
#include "base/net/loop/event_loop_group.h"
#include "base/net/rudp/rudp_endpoint.h"
#include "base/net/rudp/rudp_options.h"
#include "base/net/rudp/rudp_ticket.h"

namespace breeze
{

struct RUdpClientOptions
{
    ConnectionOptions connection; // 水位、idle_timeout
    RUdpOptions rudp;
    RUdpTicket ticket; // Start 前必须有效（这里给，或之后 SetTicket）
    std::chrono::milliseconds connect_timeout{5000};
    std::chrono::milliseconds hello_interval{200};
    std::chrono::milliseconds ping_interval{1000}; // 0 = 不发心跳
    // 握手失败时回调（IO 线程）：will_retry 恒为 false（不自动重连）。空 = 不通知（仍会记日志）。
    // 错误码：EACCES 票据不被接受、EBUSY 服务端连接表满、EPERM 被过滤、EPROTO 协议版本不符、
    // ESHUTDOWN 服务端正在停止、ETIMEDOUT 超时、ENOMEM 本地资源不足，其余是 socket 错误。
    ConnectFailedCallback on_connect_failed;
};

class RUdpClient final : public INetClient
{
public:
    RUdpClient(EventLoopGroup& group,
               const RUdpEndpoint& server,
               RUdpClientOptions options,
               IConnectionHandler& handler);
    ~RUdpClient() override;

    RUdpClient(const RUdpClient&) = delete;
    RUdpClient& operator=(const RUdpClient&) = delete;

    // 任意线程；group 必须已 Start（否则是编程错误，LOG_FATAL）。投递到所属 loop 串行处理：先 Stop 再 Start 不会并存
    // 两条连接（旧连接的 OnClosed 先于新连接的握手）。Idle 下合法；已在 Running（握手中或已连通）再调是编程错误。
    // 只返回参数类错误：选项不合法 / 票据无效 EINVAL；group 正在 / 已经停机 ESHUTDOWN。握手结果走 OnConnected /
    // on_connect_failed。
    [[nodiscard]] int Start() override;

    // 任意线程。放弃握手或关闭当前连接（等价 Close()；要优雅关闭，业务先对句柄调 CloseGracefully），回到 Idle，不等待。
    // 握手被放弃时不回调 on_connect_failed；连接的 OnClosed 仍会且仅会触发一次。对 Idle 是空操作。
    void Stop() override;

    // 换下一次 Start 用的票据（断线或握手失败之后，从控制面拿到新票据时调）。任意线程。
    // 对正在握手 / 已连通的连接没有影响。
    void SetTicket(const RUdpTicket& ticket);

    // 经当前连接发送。未连通 → Closed，不缓冲。线程安全。
    [[nodiscard]] SendResult Send(SharedBuffer payload) override;

    // 当前连接的句柄，没有连接时 raw() == 0。IO 线程在连通 / 断开时更新。
    ConnectionHandle handle() const noexcept override
    {
        return ConnectionHandle(handle_.load(std::memory_order_acquire));
    }

    // 服务端地址（构造时给的）。
    const Endpoint& endpoint() const noexcept override { return server_; }
    RUdpEndpoint server() const;

private:
    struct Impl;

    const Endpoint server_;
    std::atomic<std::uint64_t> handle_{0};
    std::unique_ptr<Impl> impl_;
};

} // namespace breeze
