#include "event_loop.h"

#include <sys/eventfd.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <iterator>

#include "connection.h"
#include "logger.h"
#include "logger_macros.h"

namespace breeze
{

EventLoop::EventLoop(NowFn now)
    : now_(std::move(now))
{
}

// 在这里定义（而不是头文件里默认）：销毁 connections_ 需要 Connection 是完整类型。
// 没有走正常关闭流程就析构时，剩下的连接直接销毁（fd 随之关闭），不再回调 OnClosed。
EventLoop::~EventLoop() = default;

int EventLoop::Init()
{
    Fd ep(::epoll_create1(EPOLL_CLOEXEC));
    if (!ep.valid())
        return errno;
    Fd wake(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
    if (!wake.valid())
        return errno;
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.ptr = &wake_handler_;
    if (::epoll_ctl(ep.get(), EPOLL_CTL_ADD, wake.get(), &ev) != 0)
        return errno;
    epfd_ = std::move(ep);
    wakefd_ = std::move(wake);
    return 0;
}

// ---- 线程 ----

namespace
{
// 当前线程正在驱动的 loop。InLoopThread 的快路径只需要一次 TLS 读加一次比较（muduo 同款）；
// 比 std::this_thread::get_id()（要调 pthread_self）与原子读省约 10 条指令，Send 每次都会检查。
thread_local const EventLoop* t_running_loop = nullptr;
} // namespace

void EventLoop::BindLoopThread()
{
    t_running_loop = this;
    const auto self = std::this_thread::get_id();
    const auto cur = loop_tid_.load(std::memory_order_relaxed);
    if (cur == self)
        return;
    if (cur != std::thread::id{})
        LOG_FATAL("EventLoop 被第二个线程驱动（Run / RunOnce 只能在同一个线程上调用）");
    loop_tid_.store(self, std::memory_order_relaxed);
}

bool EventLoop::InLoopThread() const noexcept
{
    if (t_running_loop == this)
        return true;
    const auto tid = loop_tid_.load(std::memory_order_relaxed);
    return tid == std::thread::id{} || tid == std::this_thread::get_id(); // 未开始运行：装配阶段，不限线程
}

void EventLoop::AssertInLoopThread() const
{
    if (!InLoopThread())
        LOG_FATAL("在错误的线程上使用了只能在 loop 线程使用的对象（Connection& / EventLoop）");
}

// ---- 跨线程投递与唤醒 ----

void EventLoop::Post(std::move_only_function<void()> fn)
{
    tasks_.Enqueue(Task{Task::Op::Call, std::make_unique<std::move_only_function<void()>>(std::move(fn))});
    Wake();
}

void EventLoop::Quit()
{
    // 走任务队列而不是改标志位：标志位不受"睡前复查"保护，会丢唤醒。
    tasks_.Enqueue(Task{Task::Op::Quit, nullptr});
    Wake();
}

void EventLoop::Wake()
{
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (sleeping_.exchange(false, std::memory_order_seq_cst))
    {
        const std::uint64_t one = 1;
        if (::write(wakefd_.get(), &one, sizeof(one)) != static_cast<ssize_t>(sizeof(one)) && errno != EAGAIN)
            LOG_FATAL("eventfd 写失败 errno={}", errno);
    }
}

void EventLoop::DrainWakeFd()
{
    std::uint64_t v;
    // 水平触发下不读空会一直就绪、空转。非阻塞：已经被读空时得到 EAGAIN，忽略。
    (void)!::read(wakefd_.get(), &v, sizeof(v));
}

// ---- epoll ----

int EventLoop::Add(Pollable* p, int fd, std::uint32_t events)
{
    epoll_event ev{};
    ev.events = events;
    ev.data.ptr = p;
    return ::epoll_ctl(epfd_.get(), EPOLL_CTL_ADD, fd, &ev) == 0 ? 0 : errno;
}

void EventLoop::Mod(Pollable* p, int fd, std::uint32_t events)
{
    epoll_event ev{};
    ev.events = events;
    ev.data.ptr = p;
    if (::epoll_ctl(epfd_.get(), EPOLL_CTL_MOD, fd, &ev) != 0)
        LOG_FATAL("epoll_ctl(MOD) 失败 fd={} errno={}", fd, errno);
}

void EventLoop::Del(int fd)
{
    if (::epoll_ctl(epfd_.get(), EPOLL_CTL_DEL, fd, nullptr) != 0)
        LOG_FATAL("epoll_ctl(DEL) 失败 fd={} errno={}", fd, errno);
}

// ---- 定时器 ----

EventLoop::TimerId EventLoop::RunAt(Clock::time_point when, std::move_only_function<void()> fn)
{
    AssertInLoopThread();
    const TimerId id = next_timer_id_++;
    timers_.push_back(Timer{when, id, std::move(fn)});
    std::push_heap(timers_.begin(), timers_.end(), &EventLoop::TimerLater);
    live_timers_.insert(id);
    return id;
}

EventLoop::TimerId EventLoop::RunAfter(Clock::duration delay, std::move_only_function<void()> fn)
{
    return RunAt(Now() + delay, std::move(fn));
}

void EventLoop::Cancel(TimerId id)
{
    AssertInLoopThread();
    live_timers_.erase(id); // 堆里的残留在到期或取堆顶时惰性清掉
}

void EventLoop::DropCancelledTimers()
{
    while (!timers_.empty() && live_timers_.count(timers_.front().id) == 0)
    {
        std::pop_heap(timers_.begin(), timers_.end(), &EventLoop::TimerLater);
        timers_.pop_back();
    }
}

int EventLoop::WaitMs(int timeout_ms)
{
    DropCancelledTimers();
    if (timers_.empty())
        return timeout_ms;
    const auto now = Now();
    const auto due = timers_.front().when;
    const auto ms = due <= now ? 0 : std::chrono::ceil<std::chrono::milliseconds>(due - now).count();
    const int until = static_cast<int>(std::min<long long>(ms, INT_MAX));
    return timeout_ms < 0 ? until : std::min(timeout_ms, until);
}

void EventLoop::RunTimers()
{
    if (timers_.empty())
        return;
    const auto now = Now();
    for (;;)
    {
        DropCancelledTimers();
        if (timers_.empty() || timers_.front().when > now)
            return;
        std::pop_heap(timers_.begin(), timers_.end(), &EventLoop::TimerLater);
        Timer t = std::move(timers_.back());
        timers_.pop_back();
        live_timers_.erase(t.id);
        t.fn();
    }
}

// ---- 连接持有与回收 ----

Connection* EventLoop::Adopt(std::unique_ptr<Connection> conn)
{
    Connection* raw = conn.get();
    connections_.emplace(raw, std::move(conn));
    return raw;
}

void EventLoop::QueueClose(Connection* conn) { closing_.push_back(conn); }

void EventLoop::ProcessClosing()
{
    // OnClosed 里可能又关闭别的连接，所以循环到列表为空。
    while (!closing_.empty())
    {
        std::vector<Connection*> batch;
        batch.swap(closing_);
        for (Connection* c : batch)
            c->NotifyClosed();
        for (Connection* c : batch)
            connections_.erase(c); // 销毁连接，fd 随之关闭
    }
}

// ---- 主循环 ----

void EventLoop::RunTasks(std::size_t n)
{
    // n 是睡前复查已经取出来的那一批（可能为 0）；之后继续取，直到队列空或达到本轮上限。
    // 上限防止任务不断再投递任务而饿死 IO；剩下的留给下一轮——下一轮睡前复查会取到它们，不会睡着。
    for (int round = 0;; ++round)
    {
        for (std::size_t i = 0; i < n; ++i)
        {
            Task t = std::move(batch_[i]);
            if (t.op == Task::Op::Quit)
                quit_ = true;
            else
                (*t.fn)();
        }
        if (round >= kMaxTaskRounds)
            return;
        n = tasks_.TryDequeueBulk(batch_.begin(), kTaskBatch);
        if (n == 0)
            return;
    }
}

void EventLoop::RunOnce(int timeout_ms)
{
    BindLoopThread();
    iterations_.fetch_add(1, std::memory_order_relaxed);

    // 睡前：先声明"我要睡了"，再复查队列。复查取出了任务就不睡，只做一次非阻塞的 epoll_wait。
    sleeping_.store(true, std::memory_order_seq_cst);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    const std::size_t pending = tasks_.TryDequeueBulk(batch_.begin(), kTaskBatch);
    const int wait_ms = pending > 0 ? 0 : WaitMs(timeout_ms);

    epoll_event events[kMaxEvents];
    const int n = ::epoll_wait(epfd_.get(), events, kMaxEvents, wait_ms);
    // 无论被什么唤醒，醒来先清标志，再（在分发到唤醒 fd 时）读空 eventfd。
    sleeping_.store(false, std::memory_order_seq_cst);

    for (int i = 0; i < n; ++i)
        static_cast<Pollable*>(events[i].data.ptr)->OnEvents(events[i].events);

    RunTasks(pending);
    RunTimers();
    ProcessClosing();
}

void EventLoop::Run()
{
    while (!quit_)
        RunOnce(-1);
}

} // namespace breeze
