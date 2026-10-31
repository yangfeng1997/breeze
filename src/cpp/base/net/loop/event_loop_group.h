// N 个 EventLoop + N 个线程。设计见 docs/网络库设计.md §4.1 / §4.3 / §4.8。
//
// 状态 Idle → Running → Stopping → Stopped，各转换只发生一次：Start 只能在 Idle 调用，Shutdown 之后不能再 Start
// （编程错误，LOG_FATAL）。Start / Shutdown 由同一个线程（通常是 main）调用，不并发。
// Stopping 只在 Shutdown 执行期间：running() 已是 false（拒绝新的 StreamServer / StreamClient Start），loop 仍在跑。
//
// 谁创建 IO 线程，谁在 Shutdown / 析构里 join。析构只做兜底：没 Shutdown 过就直接停（不排空）。

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "net_stats.h"

namespace breeze
{

class EventLoop;

struct EventLoopGroupOptions
{
    std::uint32_t io_threads = 1;                    // 1 到 256
    std::uint32_t max_connections_per_loop = 65536;  // 1 到 2^20；每个 loop 的连接表按它预分配（每槽 64 字节）
};

class EventLoopGroup
{
public:
    explicit EventLoopGroup(EventLoopGroupOptions options = {});
    ~EventLoopGroup();

    EventLoopGroup(const EventLoopGroup&) = delete;
    EventLoopGroup& operator=(const EventLoopGroup&) = delete;

    // 0 = 成功，否则 errno：选项越界 EINVAL；注册表已满 ENOSPC；创建 loop / 线程失败时是对应的 errno。
    // 失败时已回滚：已起的线程全部 join，已开的 fd 全部关闭，注册表名额已归还。
    [[nodiscard]] int Start();

    // 停机：拒绝新的 Start；各 loop 停掉自己的 Acceptor / Connector（StreamServer / StreamClient 经停机钩子），
    // 对所有连接 CloseGracefully，等连接表变空或到 deadline，到点强关；然后停 loop、join 线程。
    // 此间连接的 OnClosed 照常在 IO 线程回调，Shutdown 返回之后不再有任何回调；本 group 的句柄此后解析不到（Send 返回 Closed）。
    // App 只管启动不管停机，所以这条由网络库自己提供（调用方在 Run 返回之后调它）。
    // 对没有成功 Start 的对象、以及重复调用都是安全的空操作。deadline 为 0 = 不等，直接强关。
    // idle 非空时，等待期间调用方线程每轮调它一次（返回 true = 做了事，不睡直接再来）：框架层借它在停机期间继续消费事件——
    // 连接进入优雅关闭后暂停读就不再生效，主线程若只阻塞等着，对端灌进来的数据会无界堆在事件队列里。
    void Shutdown(std::chrono::milliseconds deadline, std::move_only_function<bool()> idle = {});

    // 各 loop 的计数累加。Start 之后任意线程可调（并发时各计数不是同一时刻的一致快照）。
    NetStats GetStats() const;

    bool running() const noexcept { return state_.load(std::memory_order_acquire) == State::Running; }
    // 已经在停机（Stopping）或已停机（Stopped）。StreamServer / StreamClient 的 Start 据此区分
    // "group 没起来"（编程错误）与"group 正在 / 已经停了"（回调里 Start() 恰好撞上停机，返回 ESHUTDOWN 即可）。
    // 任意线程可读。
    bool stopping() const noexcept
    {
        const State s = state_.load(std::memory_order_acquire);
        return s == State::Stopping || s == State::Stopped;
    }

private:
    friend struct GroupAccess; // 内部与测试通过 internal/core/group_access.h 取 loop

    enum class State : std::uint8_t
    {
        Idle,
        Running,
        Stopping,
        Stopped,
    };
    struct Worker; // EventLoop + Thread，定义在 .cpp

    void StopWorkers(std::size_t started);
    void DrainConnections(std::chrono::milliseconds deadline, std::move_only_function<bool()>& idle);
    bool WaitDrained(std::chrono::steady_clock::time_point until, std::move_only_function<bool()>& idle) const;

    EventLoopGroupOptions options_;
    std::atomic<State> state_{State::Idle}; // 主线程写，IO 线程（回调里的 Start）也会读
    std::uint32_t id_ = 0; // 在 GroupRegistry 里的编号
    std::vector<std::unique_ptr<Worker>> workers_;
};

} // namespace breeze
