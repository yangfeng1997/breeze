// 连接一个对端并自动重连。设计见 docs/网络库设计.md §4.1 / §4.8。
//
// 一个对象一个对端，固定在第一次 Start() 时选定的一个 loop 上（重连定时器也在那里），自己不建线程。
// 状态 Idle ⇄ Running：Start() 在 Idle 下合法，Running 下重复调用是编程错误；Stop() 回到 Idle。
// 断线 fail-fast：连接断开立即 OnClosed，随后按 ReconnectPolicy 重连（无限重试）；未连通时 Send 立即返回
// Closed，不暗中缓冲。
//
// 生命周期同 NetServer：handler 与 factory 只持引用，须比 EventLoopGroup::Shutdown 活得久；NetClient 对象自己也须覆盖到
// Shutdown 返回之后。析构时若 group 仍在运行，会先同步停掉（保险，不是用法）。

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>

#include "connection_handle.h"
#include "connection_handler.h"
#include "connection_options.h"
#include "endpoint.h"
#include "event_loop_group.h"
#include "frame_codec.h"
#include "reconnect_policy.h"
#include "send_result.h"
#include "shared_buffer.h"

namespace breeze
{

// 每次连接尝试失败时回调（IO 线程，必须非阻塞、noexcept）：OnClosed 只在连接建立之后才有，连不上要靠它观察。
// will_retry 为 false 表示遇到不可重试的错误（EACCES / EPERM / EAFNOSUPPORT / EBADF / EFAULT / ENOTSOCK / EINVAL /
// ENAMETOOLONG）：状态已回到 Idle，此后不再尝试，需要调用方显式 Start() 重来（回调里直接调 Start() 是合法的）。
// 放在 ClientOptions 而不是 ConnectionHandler：服务端也用后者。
using ConnectFailedFn = std::move_only_function<void(int sys_errno, bool will_retry) noexcept>;

struct ClientOptions
{
    ConnectionOptions connection; // 水位、idle_timeout、keepalive
    // 非阻塞 connect 的超时，到点按失败（ETIMEDOUT）重试；否则连到黑洞地址要等内核 SYN 重试约 2 分钟。
    std::chrono::milliseconds connect_timeout{5000};
    ConnectFailedFn on_connect_failed; // 空 = 不通知（仍会记日志）
};

class NetClient
{
public:
    NetClient(EventLoopGroup& group,
              const Endpoint& endpoint,
              ClientOptions options,
              ReconnectPolicy policy,
              ConnectionHandler& handler,
              const FrameCodecFactory& factory);
    ~NetClient();

    NetClient(const NetClient&) = delete;
    NetClient& operator=(const NetClient&) = delete;

    // 任意线程；group 必须已 Start（否则是编程错误，LOG_FATAL）。投递到所属 loop 串行处理，所以先 Stop 再 Start
    // 的顺序有保证， 先 Stop 再 Start 也不会并存两条连接（旧连接的 OnClosed 先于新连接的 OnConnected）。
    // 只返回参数类错误：选项不合法 EINVAL（connect_timeout ≤ 0、ReconnectPolicy::Valid() 为假）；
    // 连接建立失败走 on_connect_failed。
    [[nodiscard]] int Start();

    // 任意线程。停止重连并关闭当前连接（等价 Close()；要优雅关闭，业务先对句柄调 CloseGracefully），回到 Idle，不等待。
    // 该连接的 OnClosed 仍会且仅会触发一次，在它之后本 client 不再有任何回调。对 Idle 是空操作。
    void Stop();

    // 经当前连接发送。未连通 → Closed，不缓冲。线程安全。
    [[nodiscard]] SendResult Send(SharedBuffer payload);

    // 当前连接的句柄，没有连接时 raw() == 0。IO 线程在连接 / 断开时更新。
    ConnectionHandle handle() const noexcept { return ConnectionHandle(handle_.load(std::memory_order_acquire)); }

    const Endpoint& endpoint() const noexcept { return endpoint_; }

private:
    struct Impl;

    const Endpoint endpoint_;
    std::atomic<std::uint64_t> handle_{0};
    std::unique_ptr<Impl> impl_;
};

} // namespace breeze
