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
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "mpsc_queue.h"
#include "pollable.h"
#include "socket.h"

namespace breeze
{

class Connection;

class EventLoop
{
public:
    using Clock = std::chrono::steady_clock;
    // 时钟由 loop 注入，测试可以用假时钟；空则用 steady_clock。只在有定时器时才会调用。
    using NowFn = std::function<Clock::time_point()>;
    using TimerId = std::uint64_t; // 0 = 无效

    explicit EventLoop(NowFn now = {});
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    // 构造函数不做系统调用，fd 在这里创建。返回 errno，失败时已回滚。
    [[nodiscard]] int Init();

    // ---- 任意线程 ----

    // 投递到 loop 线程执行。fn 不能阻塞。
    void Post(std::move_only_function<void()> fn);
    // 让 Run() 返回。已投递的任务与已到期的事件在返回前照常处理。
    void Quit();

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

    // 连接归 loop 持有，由它在回收时销毁。
    Connection* Adopt(std::unique_ptr<Connection> conn);
    // 连接已标记关闭，放进回收列表：本轮分发之后才回调 OnClosed 并销毁，保证回调栈内对象仍然有效。
    void QueueClose(Connection* conn);

    std::size_t ConnectionCount() const noexcept { return connections_.size(); }
    // 已经跑过的轮数。诊断 / 测试用（比如断言 accept 遇到 EMFILE 时 loop 没有空转）；可在任意线程读。
    std::uint64_t Iterations() const noexcept { return iterations_.load(std::memory_order_relaxed); }

private:
    struct Task
    {
        enum class Op : std::uint8_t
        {
            Call,
            Quit,
        };
        Op op = Op::Call;
        // 通用投递才有这次堆分配；热路径的任务（将来的 Send 等）是不带它的小结构体。
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
    void RunTasks(std::size_t n);
    void RunTimers();
    void ProcessClosing();
    void DropCancelledTimers();

    static constexpr std::size_t kTaskBatch = 64;
    static constexpr int kMaxTaskRounds = 4; // 每轮最多处理 4 批任务
    static constexpr int kMaxEvents = 1024;

    NowFn now_;
    Fd epfd_;
    Fd wakefd_;
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

    std::unordered_map<Connection*, std::unique_ptr<Connection>> connections_;
    std::vector<Connection*> closing_;
};

} // namespace breeze
