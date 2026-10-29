// 固定大小的线程池：无 key 的任务走共享队列（哪个线程空闲谁取），有 key 的任务进该 key 对应线程的专属队列（同 key
// 串行， 即 Actor 模式）。
//
// 设计取舍（与老实现的差别）：
//   - 一把锁保护全部队列与状态，每个线程一个条件变量，不用"自旋锁 + sleeping 标志 + mutex"的三层结构：
//     任务量级是微秒以上的业务任务，锁不是瓶颈（实测见 docs/线程基础库.md），换来的是"不会丢唤醒、停机语义可证明"。
//   - 无 key 任务不是盲轮询：老实现把任务按轮询塞进某个线程的队列，那个线程被长任务占着时，后面的任务就一直排队，
//     别的线程却空闲。共享队列没有这个问题。
//   - 有 key 任务串行，也因此一个 key 的积压只会堵住该线程上的 key（以及它的 key 队列），不影响共享队列里的任务。
//   - 提交被拒绝（没 Start、正在 / 已经 Stop、队列满）返回 false / 空 optional，不崩溃：老实现在 Start 之前或 Stop
//   之后提交
//     会对 0 取模。
//   - Stop 排空：拒绝外部的新任务，但已经在队列里的、以及运行中的任务再投递的，都会执行完，保证不丢。
//     老实现里排空期间任务再投递可能落到已经退出的线程队列里，future 变 broken_promise。
//   - 队列可以设上限（max_pending），超了就拒绝，调用方决定丢弃、降级还是重试。
//   - Post 不建 future（不分配共享状态），Submit 才返回 future。
//
// 任务不应当抛异常：Post 的任务抛了会 std::terminate；Submit 的任务抛了会存进 future，由 get() 的调用方处理。
// 任务里不能等自己所在线程池的任务（会死锁）、不能在工作线程里 Stop / WaitIdle（FATAL）。
// Start / Stop 由同一个线程（通常是 main）调用，不并发；Post / Submit 任意线程。

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "thread.h"

namespace breeze
{

struct ThreadPoolOptions
{
    std::size_t threads = 1;            // 1 到 ThreadPool::kMaxThreads
    std::string name_prefix = "Worker"; // 线程名 "<前缀>-<序号>"，超过 15 个字符会被截断
    std::size_t max_pending = 0;        // 排队（还没开始执行）的任务数上限，0 = 不限
};

class ThreadPool
{
public:
    static constexpr std::size_t kMaxThreads = 64; // 空闲线程用一个 64 位掩码记录

    using Task = std::move_only_function<void()>;

    struct Stats
    {
        std::size_t threads = 0;
        std::size_t pending = 0;    // 排队中、还没开始执行
        std::uint64_t executed = 0; // 已执行完
        std::uint64_t rejected = 0; // 被拒绝的提交
    };

    explicit ThreadPool(ThreadPoolOptions options);
    ~ThreadPool(); // 没 Stop 过就先 Stop

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // 0 = 成功；选项越界 EINVAL；创建线程失败是对应的 errno，此时已回滚（已起的线程都 join 了）。只能 Start 一次（再次
    // Start 是编程错误）。
    [[nodiscard]] int Start();

    // 拒绝新的外部提交，执行完队列里与运行中任务再投递的所有任务，然后 join 线程。对没 Start / 已 Stop 的对象是空操作。
    // 没有超时：任务必须会结束。
    void Stop();

    // 等到所有已提交的任务都执行完（队列空、没有在运行的任务）。池不停。
    void WaitIdle();

    // ---- 提交：返回 false 表示被拒绝（没在运行、正在停止、队列满），任务已被销毁 ----

    // 无 key：共享队列，哪个线程空闲谁执行，不保证顺序。
    [[nodiscard]] bool Post(Task task);
    // 有 key：同一个 key 的任务在同一个线程上按提交顺序串行执行（key % 线程数 选线程）。
    [[nodiscard]] bool PostKey(std::uint64_t key, Task task);

    // 带结果。被拒绝返回空 optional。f 与 args 按值拷贝 / 移动进任务。
    template <typename F, typename... Args>
    [[nodiscard]] auto Submit(F&& f, Args&&... args)
    {
        return SubmitImpl(std::nullopt, std::forward<F>(f), std::forward<Args>(args)...);
    }
    template <typename F, typename... Args>
    [[nodiscard]] auto SubmitKey(std::uint64_t key, F&& f, Args&&... args)
    {
        return SubmitImpl(key, std::forward<F>(f), std::forward<Args>(args)...);
    }

    bool running() const noexcept;
    std::size_t thread_count() const noexcept { return options_.threads; }
    Stats stats() const;
    // 当前线程是不是本池的工作线程。
    bool InWorkerThread() const noexcept;

private:
    enum class State : std::uint8_t
    {
        Idle, // 还没 Start
        Running,
        Stopping, // 不再接受外部提交，在排空
        Stopped,
    };

    struct Worker
    {
        Thread thread;
        std::condition_variable cv;
        std::deque<Task> keyed; // 只有这个线程执行
    };

    template <typename F, typename... Args>
    auto SubmitImpl(std::optional<std::uint64_t> key, F&& f, Args&&... args)
    {
        using R = std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>;
        using Result = std::optional<std::future<R>>;
        if (!running())
        {
            NoteRejected(); // 先看一眼：池已经停了就不必建 packaged_task（它销毁时会存一个 broken_promise 异常）
            return Result{};
        }
        std::packaged_task<R()> pt(
            [fn = std::forward<F>(f), tup = std::make_tuple(std::forward<Args>(args)...)]() mutable -> R
            { return std::apply(std::move(fn), std::move(tup)); });
        std::future<R> fut = pt.get_future();
        Task task([pt = std::move(pt)]() mutable { pt(); });
        const bool ok = key ? PostKey(*key, std::move(task)) : Post(std::move(task));
        return ok ? Result{std::move(fut)} : Result{};
    }

    bool Enqueue(Task task, std::optional<std::size_t> worker);
    void NoteRejected();
    bool Accepting() const;                          // 要持锁
    void WakeFor(std::optional<std::size_t> worker); // 要持锁
    void WakeAll();                                  // 要持锁
    void WorkerLoop(std::size_t index);
    void Rollback(std::size_t started);

    const ThreadPoolOptions options_;

    mutable std::mutex mu_;
    std::atomic<State> state_{State::Idle}; // 写在 mu_ 里；running() 与 Submit 的预检查不加锁地读
    std::deque<Task> shared_;
    std::uint64_t idle_mask_ = 0; // 第 i 位 = 工作线程 i 正在等任务（且没被唤醒）
    std::size_t queued_ = 0;      // 所有队列里的任务数
    std::size_t outstanding_ = 0; // 排队 + 运行中
    std::uint64_t executed_ = 0;
    std::uint64_t rejected_ = 0;
    std::condition_variable idle_cv_; // outstanding_ 归零
    std::vector<std::unique_ptr<Worker>> workers_;
};

} // namespace breeze
