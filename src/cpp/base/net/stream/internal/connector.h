// StreamClient 在 loop 上的状态机：非阻塞 connect → 连上后建 IConnection → 断开后按退避重连。
// 设计见 docs/网络库设计.md §4.8。
//
// 全部成员只能在所属 loop 线程调用（StreamClient 把 Start / Stop 投递过来串行处理）。
// 同一时刻最多一条连接或一次连接尝试：旧连接的 OnClosed 没回调之前不开始新的尝试。

#pragma once

#include <atomic>
#include <cstdint>

#include "base/net/codec/frame_codec_interface.h"
#include "base/net/connection/connection_handler_interface.h"
#include "base/net/loop/event_loop.h"
#include "base/net/loop/pollable_interface.h"
#include "base/net/socket/unique_fd.h"
#include "base/net/stream/internal/backoff.h"
#include "base/net/stream/stream_client.h"

namespace breeze
{

class Connector final : private IPollable
{
public:
    // handle 是 StreamClient 持有的原子句柄（0 = 无连接）：IO 线程在连接 / 断开时写，任意线程的 StreamClient::Send 读。
    // handler 与 factory 只持引用。
    Connector(EventLoop& loop,
              Endpoint endpoint,
              StreamClientOptions options,
              const ReconnectPolicy& policy,
              IConnectionHandler& handler,
              const IFrameCodecFactory& factory,
              std::atomic<std::uint64_t>& handle);
    ~Connector();

    Connector(const Connector&) = delete;
    Connector& operator=(const Connector&) = delete;

    // Idle → Running，开始第一次尝试。Running 下重复调用是编程错误（LOG_FATAL）。
    void Start();
    // → Idle：取消连接尝试与退避定时器，关闭当前连接（它的 OnClosed 仍会回调一次）。Idle 下是空操作。
    void Stop();
    // 停机用：同 Stop，但不关已建立的连接（group 随后统一优雅关闭），只保证它断开后不再重连。
    void Detach();

    // 最近一次 Stop 关掉的连接，OnClosed 是否还没回调。StreamClient 析构要等它回到 false，否则 OnClosed
    // 会落在已销毁的对象上。
    bool awaiting_closed() const noexcept { return awaiting_closed_.load(std::memory_order_acquire); }

private:
    // 夹在用户 handler 与 IConnection 之间：断开时要通知 Connector 重连，其余原样转发。
    class Forwarder final : public IConnectionHandler
    {
    public:
        Forwarder(Connector& c, IConnectionHandler& user)
            : owner_(c)
            , user_(user)
        {
        }
        void OnConnected(IConnection& c) noexcept override { user_.OnConnected(c); }
        void OnMessage(IConnection& c, std::span<const std::byte> payload) noexcept override
        {
            user_.OnMessage(c, payload);
        }
        void OnClosed(IConnection& c, CloseReason r, int e) noexcept override { owner_.OnConnectionClosed(c, r, e); }

    private:
        Connector& owner_;
        IConnectionHandler& user_;
    };

    enum class Phase : std::uint8_t
    {
        Idle,
        Connecting, // 非阻塞 connect 进行中，fd_ 已登记 EPOLLOUT
        Backoff,    // 等退避定时器
        Connected,  // 有一条连接
    };

    void OnEvents(std::uint32_t events) override; // connect 完成（成功或失败）
    void Attempt();
    void OnConnectTimeout();
    void OnSocketConnected(UniqueFd fd);
    void OnConnectionClosed(IConnection& c, CloseReason reason, int sys_errno);
    void Failed(int sys_errno);
    void ScheduleRetry();
    void CancelAttempt();

    EventLoop& loop_;
    const Endpoint endpoint_;
    const ConnectionOptions conn_options_;
    const TcpOptions tcp_options_;
    const std::chrono::milliseconds connect_timeout_;
    ConnectFailedCallback on_connect_failed_;
    IConnectionHandler& user_handler_;
    const IFrameCodecFactory& factory_;
    std::atomic<std::uint64_t>& handle_;
    Forwarder forwarder_;
    Backoff backoff_;

    Phase phase_ = Phase::Idle;
    bool running_ = false;
    bool start_when_closed_ = false; // Stop 之后马上又 Start：旧连接的 OnClosed 回调之后再开始新的尝试
    std::atomic<bool> awaiting_closed_{false};
    UniqueFd fd_; // Connecting 阶段的 socket
    EventLoop::TimerId connect_timer_ = 0;
    EventLoop::TimerId retry_timer_ = 0;
};

} // namespace breeze
