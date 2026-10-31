// 一个线程上的全部 IO：epoll、跨线程投递、定时器、连接的延后回收。设计见 docs/网络库设计.md §4.5 / §4.8。
//
// 不是公开类型，放在 internal/：Run / RunOnce 不创建线程，线程由使用方（将来是 EventLoopGroup）创建并持有，
// 所以 loop 可以独立存在，测试直接在当前线程 RunOnce。
//
// 线程规则：Post / Quit 任意线程可调；其余成员只能在 loop 线程调用。loop 开始运行之前（Run / RunOnce 首次调用前）
// 是单线程的装配阶段，任何线程都可以调用；开始运行后，装配线程不能再碰它。

#pragma once

#include <sys/epoll.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "connection_handle.h"
#include "connection_options.h"
#include "connection_table.h"
#include "loop_stats.h"
#include "mpsc_queue.h"
#include "peer_info.h"
#include "pollable.h"
#include "shared_buffer.h"
#include "unique_fd.h"

namespace breeze
{

class Connection;
class ConnectionHandler;
class FrameCodec;

struct EventLoopOptions
{
    static constexpr std::uint32_t kNoGroup = UINT32_MAX;

    // 所属 group 的编号与本 loop 在 group 里的下标，填进连接句柄。独立的 loop（测试）不属于任何 group，
    // 它的连接没有可用的句柄（Connection::handle() 返回无效句柄）。
    std::uint32_t group_id = kNoGroup;
    std::uint32_t index = 0;
    // 连接表容量（连接数上限）。表按它预分配，每槽 64 字节。
    std::uint32_t max_connections = 4096;
};

class EventLoop
{
public:
    using Clock = std::chrono::steady_clock;
    // 时钟由 loop 注入，测试可以用假时钟；空则用 steady_clock。只在有定时器时才会调用。
    using NowFn = std::function<Clock::time_point()>;
    using TimerId = std::uint64_t; // 0 = 无效

    // 跨线程投递给连接的操作（ConnectionHandle 用）。不带 std::function：热路径上不多一次堆分配。
    enum class Op : std::uint8_t
    {
        Send,            // payload；发送方已把 payload.size() 计入槽的 pending_bytes
        Close,           // arg = CloseReason
        CloseGracefully, // arg = 超时毫秒
        PauseRead,
        ResumeRead,
    };
    using HookId = std::uint64_t; // 0 = 无效

    explicit EventLoop(NowFn now = {});
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    // 构造函数不做系统调用，fd 在这里创建。返回 errno，失败时已回滚。
    [[nodiscard]] int Init(const EventLoopOptions& options = {});

    // ---- 任意线程 ----

    // 投递到 loop 线程执行。fn 不能阻塞。
    void Post(std::move_only_function<void()> fn);
    // 给 handle 指向的连接投递一个操作。loop 线程执行时会按 generation 再校验一次，连接已换代则丢弃。
    void PostOp(Op op, ConnectionHandle handle, std::uint32_t arg, SharedBuffer payload);
    // 让 Run() 返回。已投递的任务与已到期的事件在返回前照常处理。
    void Quit();
    // 在 loop 线程上执行 fn 并等它返回结果。调用线程本来就是这个 loop 的线程时直接调用。
    // 只能对已有线程在跑的 loop 调用：没人运行它则永远等不到
    // （StreamServer / StreamClient 的 Start 要求 group 已 Start）。
    int CallSync(std::move_only_function<int()> fn);

    // 连接表：Find / TryReserve / Live 等任意线程可用，其余见 ConnectionTable。
    ConnectionTable& table() noexcept { return *table_; }
    const EventLoopOptions& options() const noexcept { return options_; }
    // 计数：IO 线程写，任意线程可读。写法见 loop_stats.h。
    LoopStats& stats() noexcept { return stats_; }
    const LoopStats& stats() const noexcept { return stats_; }

    // ---- 只能在 loop 线程 ----

    // 阻塞运行直到 Quit。
    void Run();
    // 跑一轮：epoll_wait（最长 timeout_ms，-1 = 一直等到有事件、有任务或最近的定时器到期）→ 分发就绪事件 → 处理任务 →
    // 定时器 → 回收已关闭的连接。
    void RunOnce(int timeout_ms);

    bool InLoopThread() const noexcept;
    void AssertInLoopThread() const;

    // epoll 注册。Add 返回 errno（新 fd 注册失败是可恢复的资源耗尽，由调用方只关这一条连接）；
    // 对已注册 fd 的 Mod / Del 失败是不可恢复的，直接 LOG_FATAL。
    [[nodiscard]] int Add(Pollable* p, int fd, std::uint32_t events);
    void Mod(Pollable* p, int fd, std::uint32_t events);
    void Del(int fd);

    // 定时器：一次性，到期在 loop 线程执行。Cancel 对已触发或不存在的 id 是空操作。
    TimerId RunAt(Clock::time_point when, std::move_only_function<void()> fn);
    TimerId RunAfter(Clock::duration delay, std::move_only_function<void()> fn);
    void Cancel(TimerId id);

    // 连接由各传输自己创建（StreamConnection::Create 等）：在 table() 里 Allocate 一个槽、Install 进去，
    // 归 loop 持有、回收时销毁。
    // 这里只提供生成句柄的原语（独立 loop，不属于任何 group 时是无效句柄），EventLoop 不认识任何具体的连接类。
    ConnectionHandle MakeHandle(std::uint32_t slot, std::uint32_t generation) const noexcept
    {
        return options_.group_id == EventLoopOptions::kNoGroup
                   ? ConnectionHandle{}
                   : ConnectionHandle::Make(options_.group_id, options_.index, slot, generation);
    }
    // 连接已标记关闭，放进回收列表：本轮分发之后才回调 OnClosed 并销毁，保证回调栈内对象仍然有效。
    void QueueClose(Connection* conn);

    // ---- 空闲检测（设计 §4.7）----
    // 登记 / 注销一条开了 idle_timeout 的连接；有登记的连接时才有扫描定时器。TouchIdle 记"收到了字节"，
    // 用本轮开头取的时间（不为每次 read 单独取时钟，精度 ≤ 1s 够用）。
    void RegisterIdle(std::uint32_t slot, std::uint32_t timeout_ms);
    void UnregisterIdle(std::uint32_t slot);
    void TouchIdle(std::uint32_t slot) noexcept { table_->idle(slot).last_active = cached_now_; }
    void SetIdlePaused(std::uint32_t slot, bool paused);

    // ---- 停机（设计 §4.8），只能在 loop 线程 ----
    // 停机钩子：停掉 Acceptor / Connector 之类"会产生新连接"的东西。BeginShutdown 时各执行一次，之后清空。
    HookId AddShutdownHook(std::move_only_function<void()> fn);
    void RemoveShutdownHook(HookId id);
    // 执行钩子，之后不再接受新连接（accepting() 变 false），并对现有连接优雅关闭；grace 为 0 则直接关。
    void BeginShutdown(std::chrono::milliseconds grace);
    // 强关所有连接（BeginShutdown 的优雅期到了还没走完时用）。
    void CloseAllConnections();
    // false = 正在停机：跨线程投递来的"新连接"任务要丢弃，归还预留名额。
    bool accepting() const noexcept { return !shutting_down_.load(std::memory_order_relaxed); }
    // BeginShutdown 已经跑完（钩子执行过、Acceptor / Connector 已停、现有连接已开始关闭）。任意线程可读：
    // EventLoopGroup 要等所有 loop 都到了这一步才停 loop，否则某个 loop 的 Acceptor 还在接新连接、
    // 投给已经退出的 loop，那个连接就永远没人服务。
    bool shutdown_started() const noexcept { return shutdown_started_.load(std::memory_order_acquire); }

    // 已预留 + 已建立的连接数，任意线程可读。
    std::size_t ConnectionCount() const noexcept { return table_->Live(); }
    // 已经跑过的轮数。诊断 / 测试用（比如断言 accept 遇到 EMFILE 时 loop 没有空转）；可在任意线程读。
    std::uint64_t Iterations() const noexcept { return iterations_.load(std::memory_order_relaxed); }

private:
    struct Task
    {
        enum class Kind : std::uint8_t
        {
            Call, // fn
            Quit,
            Conn, // 对连接的操作：op / handle / arg / payload
        };
        Kind kind = Kind::Call;
        Op op = Op::Send;
        std::uint32_t arg = 0;
        ConnectionHandle handle;
        SharedBuffer payload;
        // 通用投递才有这次堆分配；热路径上的 Conn 任务不带它。
        std::unique_ptr<std::move_only_function<void()>> fn;
    };

    struct Timer
    {
        Clock::time_point when;
        TimerId id;
        std::move_only_function<void()> fn;
    };

    class WakeHandler final : public Pollable
    {
    public:
        explicit WakeHandler(EventLoop& loop)
            : loop_(loop)
        {
        }
        void OnEvents(std::uint32_t) override { loop_.DrainWakeFd(); }

    private:
        EventLoop& loop_;
    };

    // 最小堆：when 小的在堆顶。
    static bool TimerLater(const Timer& a, const Timer& b) { return a.when > b.when; }

    Clock::time_point Now() const { return now_ ? now_() : Clock::now(); }
    void BindLoopThread();
    void Wake();
    void DrainWakeFd();
    int WaitMs(int timeout_ms);
    void RunTask(Task& t);
    void RunTasks(std::size_t n);
    void RunTimers();
    void ProcessClosing();
    void DropCancelledTimers();
    void ScanIdle();
    void ArmIdleScan();

    // 空闲扫描：每 kIdleScanInterval 扫 1/kIdleScanSlices 的槽，所以每个槽 ≤ 1s 被检查一次。
    static constexpr std::chrono::milliseconds kIdleScanInterval{100};
    static constexpr std::uint32_t kIdleScanSlices = 10;

    static constexpr std::size_t kTaskBatch = 64;
    static constexpr int kMaxTaskRounds = 4; // 每轮最多处理 4 批任务
    static constexpr int kMaxEvents = 1024;

    NowFn now_;
    EventLoopOptions options_;
    std::unique_ptr<ConnectionTable> table_;
    UniqueFd epfd_;
    UniqueFd wakefd_;
    WakeHandler wake_handler_{*this};

    // 唤醒协议（Dekker 式握手，两侧都用 seq_cst，见 docs/网络库设计.md §4.5）：
    //   消费者：sleeping = true → fence → 复查任务队列 → 再睡
    //   生产者：入队 → fence → if (sleeping.exchange(false)) 写 eventfd
    std::atomic<bool> sleeping_{false};
    MpscQueue<Task> tasks_;
    std::array<Task, kTaskBatch> batch_; // loop 线程专用，复用避免每轮构造

    std::atomic<std::thread::id> loop_tid_{};
    std::atomic<std::uint64_t> iterations_{0};
    bool quit_ = false;

    std::vector<Timer> timers_; // 最小堆，按 when
    std::unordered_set<TimerId> live_timers_;
    TimerId next_timer_id_ = 1;

    std::vector<Connection*> closing_;

    LoopStats stats_;

    // 空闲检测：登记了 idle_timeout 的连接数；cached_now_ 在 idle_active_ > 0 时每轮开头刷新。
    std::uint32_t idle_active_ = 0;
    std::uint32_t idle_cursor_ = 0;
    TimerId idle_timer_ = 0;
    Clock::time_point cached_now_{};

    std::atomic<bool> shutting_down_{false};    // BeginShutdown 一开始就置位：之后不再接受新连接
    std::atomic<bool> shutdown_started_{false}; // BeginShutdown 结束时置位
    std::vector<std::pair<HookId, std::move_only_function<void()>>> hooks_;
    HookId next_hook_id_ = 1;
};

} // namespace breeze
