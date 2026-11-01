// 把 IO 线程上的连接事件回投到 App 主循环，保持"主循环单线程、业务无锁"。设计见 docs/网络库设计.md §5。
//
//   IO 线程 N ──IConnectionHandler──▶ 事件入该线程专属的 SpscQueue ──App::Notify──▶ 主循环 Drain
//   后回调业务（IMainLoopHandler）
//
// - 事件队列每个 IO 线程一条（按连接句柄里的 group / loop 定位）；Connected → Message* → Closed 走同一条，不分两道，
//   所以 Closed 不会超车。Enqueue 无界、不阻塞。
// - 入口背压 = PauseRead，不是阻塞线程：每连接在途上限（条数 + 字节）与全局入站字节预算，超限就摘掉该连接的 EPOLLIN，
//   消费到低水位（高水位的一半，留出滞后区间避免来回 epoll_ctl）再恢复。暂停是软的：本次 read 已读进来的帧照常入队
//   （最多 64KB），所以实际在途量最多超出上限一次 read。
// - 主循环线程 = 调 Drain 的线程，必须固定（队列是 SPSC，Debug 会断言）。
//
// 生命周期：Wrap 出来的 IConnectionHandler 归本对象所有；本对象必须比 EventLoopGroup::Shutdown 活得久（Shutdown
// 之后不再有 IO 线程 往队列里写）。析构时未消费的事件直接丢弃，不回调业务。

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

#include "base/net/connection/connection_handle.h"
#include "base/net/connection/connection_handler_interface.h"
#include "base/queue/spsc_queue.h"
#include "framework/app/app.h"
#include "framework/net/main_loop_handler_interface.h"

namespace breeze
{

struct InboundLimits
{
    std::size_t max_messages_per_conn = 256;
    std::size_t max_bytes_per_conn = 64 * 1024; // 要小：10 万连接 × 256KB ≈ 25GB
    std::size_t global_budget_bytes = 256ull * 1024 * 1024;
};

struct ExecutorStats
{
    std::uint64_t queued_events = 0;  // 各队列里尚未分发的事件数（近似）
    std::uint64_t queue_peak = 0;     // 单条队列的历史峰值（近似，消费者在每次批量出队前采样）
    std::uint64_t inflight_bytes = 0; // 已入队、未分发的 payload 字节，全局合计
    std::uint64_t pauses = 0;         // 因在途上限而 PauseRead 的累计次数
    std::uint64_t dispatched = 0;     // 主循环已分发的事件数
};

class MainLoopExecutor
{
public:
    explicit MainLoopExecutor(App& app, InboundLimits limits = {});
    ~MainLoopExecutor();

    MainLoopExecutor(const MainLoopExecutor&) = delete;
    MainLoopExecutor& operator=(const MainLoopExecutor&) = delete;

    // 只能在还没有连接的时候调（装配阶段）：IO 线程不加锁地读它。
    void SetLimits(const InboundLimits& limits) { limits_ = limits; }
    const InboundLimits& limits() const noexcept { return limits_; }

    // 交给 StreamServer / StreamClient 构造。同一个 handler 重复 Wrap 会得到不同的适配器，行为相同。
    IConnectionHandler& Wrap(IMainLoopHandler& handler);

    // 在主循环线程上分发至多 limit 个事件；返回 true = 可能仍有积压（分发了 limit 个），false = 已空。
    // 各队列轮转着取，一条队列的积压不会饿死别的。
    bool Drain(std::size_t limit);

    ExecutorStats GetStats() const;

private:
    struct ConnState;
    struct StateContext;
    struct Event;
    class Adapter;
    using Queue = SpscQueue<Event>;

    static constexpr std::size_t kMaxQueues = ConnectionHandle::kMaxGroups * ConnectionHandle::kMaxLoops;
    static constexpr std::size_t kBatch = 64;

    // ---- IO 线程 ----
    void OnConnected(IConnection& c, IMainLoopHandler& h);
    void OnMessage(IConnection& c, std::span<const std::byte> payload, IMainLoopHandler& h);
    void OnClosed(IConnection& c, CloseReason reason, int sys_errno, IMainLoopHandler& h);
    void Push(ConnectionHandle h, Event&& ev);
    Queue& QueueFor(ConnectionHandle h);
    void MaybePause(IConnection& c, const std::shared_ptr<ConnState>& st);

    // ---- 主循环线程 ----
    void Dispatch(Event& ev);
    void Release(const std::shared_ptr<ConnState>& st, std::size_t len);
    void TryResume(const std::shared_ptr<ConnState>& st);
    void ResumeGlobalWaiters();

    // ---- 两边都用 ----
    bool ConnOver(const ConnState& st, bool low) const noexcept;
    bool StillOver(const ConnState& st) const noexcept;
    std::uint64_t GlobalLow() const noexcept { return limits_.global_budget_bytes / 2; }
    void AddWaiter(const std::shared_ptr<ConnState>& st);

    App& app_;
    InboundLimits limits_;

    // 队列按 (group, loop) 懒创建，由该 IO 线程自己创建；table_ 供生产者查找，active_ 供消费者遍历（创建在 create_mu_
    // 下，少见）。
    std::array<std::atomic<Queue*>, kMaxQueues> table_{};
    std::array<std::atomic<Queue*>, kMaxQueues> active_{};
    std::atomic<std::size_t> active_count_{0};
    std::mutex create_mu_;
    std::vector<std::unique_ptr<Queue>> owned_;

    std::vector<std::unique_ptr<Adapter>> adapters_; // Wrap 只在装配阶段调

    // 全局入站预算：在途字节。受限的连接在 waiters_ 里排队，全局降到低水位时由主线程统一恢复。
    std::atomic<std::uint64_t> global_bytes_{0};
    std::mutex waiters_mu_;
    std::vector<std::shared_ptr<ConnState>> waiters_;
    std::atomic<std::size_t> waiters_count_{0};

    std::atomic<std::uint64_t> pauses_{0};
    std::atomic<std::uint64_t> dispatched_{0};

    std::unique_ptr<Event[]> batch_; // 主循环线程专用，复用
    std::size_t rr_ = 0;             // Drain 的轮转起点，主循环线程专用
};

} // namespace breeze
