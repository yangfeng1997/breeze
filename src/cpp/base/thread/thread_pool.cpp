#include "base/thread/thread_pool.h"

#include <bit>
#include <cerrno>

#include "base/logger/logger.h"
#include "base/logger/logger_macros.h"

namespace breeze
{

namespace
{
// 当前线程属于哪个池（工作线程里才非空）：排空期间允许工作线程里的任务再投递，也据此识别 Stop / WaitIdle 的误用。
thread_local const ThreadPool* t_pool = nullptr;
} // namespace

ThreadPool::ThreadPool(ThreadPoolOptions options)
    : options_(std::move(options))
{
}

ThreadPool::~ThreadPool() { Stop(); }

bool ThreadPool::InWorkerThread() const noexcept { return t_pool == this; }

bool ThreadPool::running() const noexcept { return state_.load(std::memory_order_acquire) == State::Running; }

ThreadPool::Stats ThreadPool::stats() const
{
    const std::lock_guard lk(mu_);
    return {options_.threads, queued_, executed_, rejected_};
}

// ---- 生命周期 ----

int ThreadPool::Start()
{
    if (options_.threads < 1 || options_.threads > kMaxThreads)
        return EINVAL;
    {
        const std::lock_guard lk(mu_);
        if (state_ != State::Idle)
            LOG_FATAL("ThreadPool 只能 Start 一次（Stop 之后也不能再 Start）prefix={}", options_.name_prefix);
    }
    // 先把 Worker 都建好、再起线程：中途失败时只有"已起的线程"需要 join。
    for (std::size_t i = 0; i < options_.threads; ++i)
        workers_.push_back(std::make_unique<Worker>());
    for (std::size_t i = 0; i < workers_.size(); ++i)
    {
        const std::string name = options_.name_prefix + "-" + std::to_string(i);
        if (const int e = workers_[i]->thread.Start([this, i] { WorkerLoop(i); }, name); e != 0)
        {
            Rollback(i);
            return e;
        }
    }
    const std::lock_guard lk(mu_);
    state_ = State::Running;
    return 0;
}

// 已起的 started 个线程此时都在空等（队列是空的）：置 Stopping 把它们叫醒，它们看到"停止且没有未完成的任务"就退出。
void ThreadPool::Rollback(std::size_t started)
{
    {
        const std::lock_guard lk(mu_);
        state_ = State::Stopping;
        WakeAll();
    }
    for (std::size_t i = 0; i < started; ++i)
        workers_[i]->thread.Join();
    workers_.clear();
    const std::lock_guard lk(mu_);
    state_ = State::Idle;
}

void ThreadPool::Stop()
{
    {
        const std::lock_guard lk(mu_);
        if (state_ != State::Running)
            return;
        if (InWorkerThread())
            LOG_FATAL("ThreadPool::Stop 不能在它自己的工作线程里调用（会 join 自己）prefix={}", options_.name_prefix);
        state_ = State::Stopping;
        if (outstanding_ == 0)
            WakeAll();
    }
    for (auto& w : workers_)
        w->thread.Join();
    const std::lock_guard lk(mu_);
    state_ = State::Stopped;
}

void ThreadPool::WaitIdle()
{
    if (InWorkerThread())
        LOG_FATAL("ThreadPool::WaitIdle 不能在工作线程里调用（会等自己）prefix={}", options_.name_prefix);
    std::unique_lock lk(mu_);
    idle_cv_.wait(lk, [this] { return outstanding_ == 0; });
}

// ---- 提交 ----

// 外部只在 Running 时接受；Stopping 时只接受本池工作线程里的任务再投递——它们是"运行中的任务"，
// outstanding_ 还算着它们，工作线程不会在它们结束之前退出，所以不会丢。
bool ThreadPool::Accepting() const
{
    return state_ == State::Running || (state_ == State::Stopping && InWorkerThread());
}

void ThreadPool::NoteRejected()
{
    const std::lock_guard lk(mu_);
    ++rejected_;
}

bool ThreadPool::Post(Task task) { return Enqueue(std::move(task), std::nullopt); }

bool ThreadPool::PostKey(std::uint64_t key, Task task)
{
    return Enqueue(std::move(task), static_cast<std::size_t>(key % options_.threads));
}

bool ThreadPool::Enqueue(Task task, std::optional<std::size_t> worker)
{
    if (!task)
        LOG_FATAL("ThreadPool 提交了空任务 prefix={}", options_.name_prefix);
    const std::lock_guard lk(mu_);
    if (!Accepting() || (options_.max_pending != 0 && queued_ >= options_.max_pending))
    {
        ++rejected_;
        return false; // task 随参数销毁
    }
    if (worker)
        workers_[*worker]->keyed.push_back(std::move(task));
    else
        shared_.push_back(std::move(task));
    ++queued_;
    ++outstanding_;
    WakeFor(worker);
    return true;
}

// 持锁时唤醒：notify 在锁内发出，Stop 的 join 要等工作线程拿到这把锁，所以被唤醒的 Worker 一定还活着。
void ThreadPool::WakeFor(std::optional<std::size_t> worker)
{
    std::size_t target;
    if (worker)
    {
        target = *worker;
        if (!(idle_mask_ & (std::uint64_t{1} << target)))
            return; // 它正忙：做完手上的会回到循环顶部看队列
    }
    else
    {
        if (idle_mask_ == 0)
            return; // 都在忙：谁先做完谁来共享队列里取
        target = static_cast<std::size_t>(std::countr_zero(idle_mask_));
    }
    idle_mask_ &= ~(std::uint64_t{1} << target); // 摘掉空闲位：同时到的另一个提交会挑下一个空闲线程，不会重复叫醒同一个
    workers_[target]->cv.notify_one();
}

void ThreadPool::WakeAll()
{
    for (auto& w : workers_)
        w->cv.notify_one();
}

// ---- 工作线程 ----

void ThreadPool::WorkerLoop(std::size_t index)
{
    t_pool = this;
    Worker& self = *workers_[index];
    const std::uint64_t bit = std::uint64_t{1} << index;
    std::unique_lock lk(mu_);
    for (;;)
    {
        Task task;
        if (!self.keyed.empty()) // 只有本线程能做的优先：共享队列里的别的线程也能取
        {
            task = std::move(self.keyed.front());
            self.keyed.pop_front();
        }
        else if (!shared_.empty())
        {
            task = std::move(shared_.front());
            shared_.pop_front();
        }
        else
        {
            if (state_ == State::Stopping && outstanding_ == 0)
                return; // 停止，且没有排队的、也没有运行中的任务（它们才可能再投递）
            idle_mask_ |= bit;
            self.cv.wait(lk); // 虚假唤醒与"被别人抢先取走"都无妨：回到循环顶部重新判断
            idle_mask_ &= ~bit;
            continue;
        }
        --queued_;
        lk.unlock();
        task();         // 不带锁地执行；任务里可以再 Post
        task = nullptr; // 任务捕获的状态在锁外销毁
        lk.lock();
        ++executed_;
        if (--outstanding_ == 0)
        {
            idle_cv_.notify_all();
            if (state_ == State::Stopping)
                WakeAll(); // 最后一个未完成的任务做完了：叫醒所有线程去退出
        }
    }
}

} // namespace breeze
