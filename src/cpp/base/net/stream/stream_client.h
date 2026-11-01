// TCP 与 UDS 共用的客户端基类：连接一个对端并自动重连。设计见 docs/网络库设计.md §4.1 / §4.8。
// 业务代码用 TcpClient（tcp/）或 UdsClient（uds/），它们收类型化的端点与选项；这个基类收通用的 Endpoint 与
// StreamClientOptions，给"按端点种类分派"的代码用（NetModule、测试），也是它们的统一句柄。
//
// 一个对象一个对端，固定在第一次 Start() 时选定的一个 loop 上（重连定时器也在那里），自己不建线程。
// 状态 Idle ⇄ Running：Start() 在 Idle 下合法，Running 下重复调用是编程错误；Stop() 回到 Idle。
// 断线 fail-fast：连接断开立即 OnClosed，随后按 ReconnectPolicy 重连（无限重试）；未连通时 Send 立即返回
// Closed，不暗中缓冲。
//
// 生命周期同 StreamServer：handler 与 factory 只持引用，须比 EventLoopGroup::Shutdown 活得久；
// StreamClient 对象自己也须覆盖到 Shutdown 返回之后。析构时若 group 仍在运行，会先同步停掉（保险，不是用法）。

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>

#include "base/buffer/shared_buffer.h"
#include "base/net/codec/frame_codec_interface.h"
#include "base/net/connection/connection_handle.h"
#include "base/net/connection/connection_handler_interface.h"
#include "base/net/connection/connection_options.h"
#include "base/net/connection/net_client_interface.h"
#include "base/net/connection/send_result.h"
#include "base/net/endpoint/endpoint.h"
#include "base/net/loop/event_loop_group.h"
#include "base/net/stream/reconnect_policy.h"
#include "base/net/tcp/tcp_options.h"

namespace breeze
{

struct StreamClientOptions
{
    ConnectionOptions connection; // 水位、idle_timeout
    TcpOptions tcp;               // keepalive 等：只对 TCP 端点生效，UDS 端点不看它
    // 非阻塞 connect 的超时，到点按失败（ETIMEDOUT）重试；否则连到黑洞地址要等内核 SYN 重试约 2 分钟。
    std::chrono::milliseconds connect_timeout{5000};
    ConnectFailedCallback on_connect_failed; // 空 = 不通知（仍会记日志）
};

class StreamClient : public INetClient
{
public:
    StreamClient(EventLoopGroup& group,
                 const Endpoint& endpoint,
                 StreamClientOptions options,
                 ReconnectPolicy policy,
                 IConnectionHandler& handler,
                 const IFrameCodecFactory& factory);
    ~StreamClient() override; // TcpClient / UdsClient 经基类指针销毁

    StreamClient(const StreamClient&) = delete;
    StreamClient& operator=(const StreamClient&) = delete;

    // 任意线程；group 必须已 Start（否则是编程错误，LOG_FATAL）。投递到所属 loop 串行处理，所以先 Stop 再 Start
    // 的顺序有保证， 先 Stop 再 Start 也不会并存两条连接（旧连接的 OnClosed 先于新连接的 OnConnected）。
    // 只返回参数类错误：选项不合法 EINVAL（connect_timeout ≤ 0、ReconnectPolicy::Valid() 为假）、
    // 端点是 UDP（数据报）返回 EPROTONOSUPPORT；
    // 连接建立失败走 on_connect_failed。group 正在 / 已经停机返回 ESHUTDOWN（回调里 Start()
    // 恰好撞上停机时不会让进程中止）。
    [[nodiscard]] int Start() override;

    // 任意线程。停止重连并关闭当前连接（等价 Close()；要优雅关闭，业务先对句柄调 CloseGracefully），回到 Idle，不等待。
    // 该连接的 OnClosed 仍会且仅会触发一次，在它之后本 client 不再有任何回调。对 Idle 是空操作。
    void Stop() override;

    // 经当前连接发送。未连通 → Closed，不缓冲。线程安全。
    [[nodiscard]] SendResult Send(SharedBuffer payload) override;

    // 当前连接的句柄，没有连接时 raw() == 0。IO 线程在连接 / 断开时更新。
    ConnectionHandle handle() const noexcept override
    {
        return ConnectionHandle(handle_.load(std::memory_order_acquire));
    }

    const Endpoint& endpoint() const noexcept override { return endpoint_; }

private:
    struct Impl;

    const Endpoint endpoint_;
    std::atomic<std::uint64_t> handle_{0};
    std::unique_ptr<Impl> impl_;
};

} // namespace breeze
