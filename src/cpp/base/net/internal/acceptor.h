// 监听一个 endpoint，接受连接并交给回调。设计见 docs/网络库设计.md §4.8 / §4.11。
//
// 只能在所属 EventLoop 的线程上使用（loop 开始运行之前的装配阶段不限）。

#pragma once

#include <sys/socket.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>

#include "endpoint.h"
#include "event_loop.h"
#include "pollable.h"
#include "socket.h"

namespace breeze
{

class Acceptor final : private Pollable
{
public:
    // 每接受一条连接调用一次；peer 为空表示对端未命名（UDS 客户端通常如此）。
    using OnAccept = std::move_only_function<void(Fd, std::optional<Endpoint>)>;

    Acceptor(EventLoop& loop, Endpoint endpoint, OnAccept on_accept);
    ~Acceptor();

    Acceptor(const Acceptor&) = delete;
    Acceptor& operator=(const Acceptor&) = delete;

    // 创建 socket、bind、listen 并登记到 loop。返回 errno（0 = 成功），失败时已回滚（不残留 fd）。
    // UDS 路径会先探测残留文件（见 sock::Bind）。
    [[nodiscard]] int Listen(int backlog = SOMAXCONN);
    // 停止接受并关闭监听 fd；UDS 路径随之 unlink（是本对象 bind 出来的）。重复调用是空操作。
    void Stop();

    // 实际绑定的地址：端口填 0 时这里是系统分配的端口。
    const Endpoint& endpoint() const noexcept { return endpoint_; }

    // 因 fd 耗尽（EMFILE / ENFILE）而被接受后立即关闭的连接数。
    // 可在任意线程读（测试用）。
    std::uint64_t rejected_fd_exhausted() const noexcept
    {
        return rejected_fd_exhausted_.load(std::memory_order_relaxed);
    }

private:
    void OnEvents(std::uint32_t events) override;
    void HandleFdExhausted();

    // 每次就绪最多接受这么多条：水平触发下剩下的会再次就绪，不让一次涌入的连接独占 loop。
    static constexpr int kMaxAcceptPerEvent = 64;

    EventLoop& loop_;
    Endpoint endpoint_;
    OnAccept on_accept_;
    Fd listen_fd_;
    Fd idle_fd_; // 预留的空闲 fd（/dev/null）：fd 耗尽时腾出一个名额，把待接受的连接接出来关掉
    bool listening_ = false;
    std::atomic<std::uint64_t> rejected_fd_exhausted_{0};
    EventLoop::Clock::time_point last_exhausted_log_{};
};

} // namespace breeze
