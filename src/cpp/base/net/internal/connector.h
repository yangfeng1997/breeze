// NetClient 在 loop 上的状态机：非阻塞 connect → 连上后建 Connection → 断开后按退避重连。
// 设计见 docs/网络库设计.md §4.8。
//
// 全部成员只能在所属 loop 线程调用（NetClient 把 Start / Stop 投递过来串行处理）。
// 同一时刻最多一条连接或一次连接尝试：旧连接的 OnClosed 没回调之前不开始新的尝试。

#pragma once

#include <atomic>
#include <cstdint>

#include "backoff.h"
#include "connection_handler.h"
#include "event_loop.h"
#include "frame_codec.h"
#include "net_client.h"
#include "pollable.h"
#include "socket.h"

namespace breeze
{

class Connector final : private Pollable
{
public:
    // handle 是 NetClient 持有的原子句柄（0 = 无连接）：IO 线程在连接 / 断开时写，任意线程的 NetClient::Send 读。
    // handler 与 factory 只持引用。
    Connector(EventLoop& loop,
              Endpoint endpoint,
              ClientOptions options,
              const ReconnectPolicy& policy,
              ConnectionHandler& handler,
              const FrameCodecFactory& factory,
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

    // 最近一次 Stop 关掉的连接，OnClosed 是否还没回调。NetClient 析构要等它回到 false，否则 OnClosed
    // 会落在已销毁的对象上。
    bool awaiting_closed() const noexcept { return awaiting_closed_.load(std::memory_order_acquire); }

private:
    // 夹在用户 handler 与 Connection 之间：断开时要通知 Connector 重连，其余原样转发。
    class Forwarder final : public ConnectionHandler
    {
    public:
        Forwarder(Connector& c, ConnectionHandler& user)
            : owner_(c)
            , user_(user)
        {
        }
        void OnConnected(Connection& c) noexcept override { user_.OnConnected(c); }
        void OnMessage(Connection& c, std::span<const std::byte> payload) noexcept override
        {
            user_.OnMessage(c, payload);
        }
        void OnClosed(Connection& c, CloseReason r, int e) noexcept override { owner_.OnConnectionClosed(c, r, e); }

    private:
        Connector& owner_;
        ConnectionHandler& user_;
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
    void OnSocketConnected(Fd fd);
    void OnConnectionClosed(Connection& c, CloseReason reason, int sys_errno);
    void Failed(int sys_errno);
    void ScheduleRetry();
    void CancelAttempt();

    EventLoop& loop_;
    const Endpoint endpoint_;
    const ConnectionOptions conn_options_;
    const std::chrono::milliseconds connect_timeout_;
    ConnectFailedFn on_connect_failed_;
    ConnectionHandler& user_handler_;
    const FrameCodecFactory& factory_;
    std::atomic<std::uint64_t>& handle_;
    Forwarder forwarder_;
    Backoff backoff_;

    Phase phase_ = Phase::Idle;
    bool running_ = false;
    bool start_when_closed_ = false; // Stop 之后马上又 Start：旧连接的 OnClosed 回调之后再开始新的尝试
    std::atomic<bool> awaiting_closed_{false};
    Fd fd_; // Connecting 阶段的 socket
    EventLoop::TimerId connect_timer_ = 0;
    EventLoop::TimerId retry_timer_ = 0;
};

} // namespace breeze
