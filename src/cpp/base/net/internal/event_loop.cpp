#include "event_loop.h"

#include <sys/eventfd.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <iterator>
#include <latch>

#include "connection.h"
#include "logger.h"
#include "logger_macros.h"

namespace breeze
{

EventLoop::EventLoop(NowFn now)
    : now_(std::move(now))
{
}

// 在这里定义（而不是头文件里默认）：销毁连接表需要 Connection 是完整类型。
// 没有走正常关闭流程就析构时，剩下的连接直接销毁（fd 随之关闭），不再回调 OnClosed。
EventLoop::~EventLoop() = default;

int EventLoop::Init(const LoopOptions& options)
{
    if (options.max_connections == 0 || options.max_connections > ConnectionTable::kMaxCapacity)
        return EINVAL;
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
    options_ = options;
    table_ = std::make_unique<ConnectionTable>(options.max_connections);
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
    Task t;
    t.kind = Task::Kind::Call;
    t.fn = std::make_unique<std::move_only_function<void()>>(std::move(fn));
    tasks_.Enqueue(std::move(t));
    Wake();
}

void EventLoop::PostOp(Op op, ConnectionHandle handle, std::uint32_t arg, SharedBuffer payload)
{
    Task t;
    t.kind = Task::Kind::Conn;
    t.op = op;
    t.arg = arg;
    t.handle = handle;
    t.payload = std::move(payload);
    tasks_.Enqueue(std::move(t));
    Wake();
}

void EventLoop::Quit()
{
    // 走任务队列而不是改标志位：标志位不受"睡前复查"保护，会丢唤醒。
    Task t;
    t.kind = Task::Kind::Quit;
    tasks_.Enqueue(std::move(t));
    Wake();
}

int EventLoop::CallSync(std::move_only_function<int()> fn)
{
    // 只认"确实正在这个 loop 的线程里"，不能用 InLoopThread()：它对"loop 还没开始运行"一律放行（装配阶段的宽松语义），
    // 而 group 的线程已经创建、只是还没跑到第一轮时，调用线程会被误判成 loop 线程。
    if (t_running_loop == this)
        return fn();
    int rc = 0;
    std::latch done(1);
    Post(
        [&]
        {
            rc = fn();
            done.count_down();
        });
    done.wait();
    return rc;
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

Connection* EventLoop::NewConnection(Fd fd,
                                     PeerInfo peer,
                                     ConnectionHandler& handler,
                                     std::unique_ptr<FrameCodec> codec,
                                     const ConnectionOptions& options)
{
    AssertInLoopThread();
    const std::uint32_t slot = table_->Allocate(options.high_water_mark);
    const std::uint32_t generation = table_->slot(slot).generation.load(std::memory_order_relaxed);
    const ConnectionHandle handle = options_.group_id == LoopOptions::kNoGroup
                                        ? ConnectionHandle{}
                                        : ConnectionHandle::Make(options_.group_id, options_.index, slot, generation);
    std::unique_ptr<Connection> conn(
        new Connection(*this, std::move(fd), std::move(peer), handler, std::move(codec), options, table_->slot(slot), slot, handle));
    Connection* raw = conn.get();
    table_->Install(slot, std::move(conn));
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
            table_->Reclaim(c->slot_index_); // 销毁连接（fd 随之关闭），槽的 generation + 1
    }
}

// ---- 空闲检测 ----

void EventLoop::ArmIdleScan()
{
    if (idle_timer_ == 0)
        idle_timer_ = RunAfter(kIdleScanInterval, [this] { ScanIdle(); });
}

void EventLoop::IdleRegister(std::uint32_t slot, std::uint32_t timeout_ms)
{
    AssertInLoopThread();
    auto& e = table_->idle(slot);
    cached_now_ = Now();
    e.last_active = cached_now_;
    e.timeout_ms = timeout_ms;
    e.paused = false;
    ++idle_active_;
    ArmIdleScan();
}

void EventLoop::IdleUnregister(std::uint32_t slot)
{
    auto& e = table_->idle(slot);
    if (e.timeout_ms == 0)
        return;
    e.timeout_ms = 0;
    --idle_active_; // 扫描定时器下次触发时发现没有登记的连接，就不再重装
}

void EventLoop::IdleSetPaused(std::uint32_t slot, bool paused)
{
    auto& e = table_->idle(slot);
    if (e.timeout_ms == 0)
        return;
    e.paused = paused;
    if (!paused)
        e.last_active = Now(); // 恢复读时重新计时：过载期间的暂停不算空闲
}

void EventLoop::ScanIdle()
{
    idle_timer_ = 0;
    if (idle_active_ == 0)
        return;
    const auto now = Now();
    const std::uint32_t cap = table_->capacity();
    const std::uint32_t chunk = (cap + kIdleScanSlices - 1) / kIdleScanSlices;
    for (std::uint32_t i = 0; i < chunk; ++i)
    {
        const std::uint32_t slot = idle_cursor_;
        if (++idle_cursor_ == cap)
            idle_cursor_ = 0;
        const auto& e = table_->idle(slot);
        if (e.timeout_ms == 0 || e.paused || now - e.last_active < std::chrono::milliseconds(e.timeout_ms))
            continue;
        if (Connection* c = table_->At(slot))
            c->CloseIdle();
    }
    ArmIdleScan();
}

// ---- 停机 ----

EventLoop::HookId EventLoop::AddShutdownHook(std::move_only_function<void()> fn)
{
    AssertInLoopThread();
    const HookId id = next_hook_id_++;
    hooks_.emplace_back(id, std::move(fn));
    return id;
}

void EventLoop::RemoveShutdownHook(HookId id)
{
    AssertInLoopThread();
    std::erase_if(hooks_, [id](const auto& h) { return h.first == id; });
}

void EventLoop::BeginShutdown(std::chrono::milliseconds grace)
{
    AssertInLoopThread();
    shutting_down_.store(true, std::memory_order_relaxed);
    // 钩子先搬出来再跑：钩子里调 RemoveShutdownHook 不会破坏正在遍历的容器。
    auto hooks = std::move(hooks_);
    hooks_.clear();
    for (auto& h : hooks)
        h.second();
    table_->ForEachConnection(
        [&](Connection& c)
        {
            if (grace.count() == 0)
                c.Close();
            else
                c.CloseGracefully(grace);
        });
    shutdown_started_.store(true, std::memory_order_release);
}

void EventLoop::CloseAllConnections()
{
    AssertInLoopThread();
    table_->ForEachConnection([](Connection& c) { c.Close(); });
}

// ---- 主循环 ----

void EventLoop::RunTask(Task& t)
{
    switch (t.kind)
    {
        case Task::Kind::Call:
            (*t.fn)();
            break;
        case Task::Kind::Quit:
            quit_ = true;
            break;
        case Task::Kind::Conn:
        {
            // 第二道校验：句柄在投递之后、执行之前可能已经换代，迟到的任务丢弃（payload 随 t 释放）。
            Connection* c = table_->Current(t.handle.slot(), t.handle.generation());
            if (c == nullptr)
                break;
            switch (t.op)
            {
                case Op::Send:
                    c->SendFromTask(std::move(t.payload));
                    break;
                case Op::Close:
                    c->CloseFromTask(static_cast<CloseReason>(t.arg));
                    break;
                case Op::CloseGracefully:
                    c->CloseGracefully(std::chrono::milliseconds(t.arg));
                    break;
                case Op::PauseRead:
                    c->PauseRead();
                    break;
                case Op::ResumeRead:
                    c->ResumeRead();
                    break;
            }
            break;
        }
    }
}

void EventLoop::RunTasks(std::size_t n)
{
    // n 是睡前复查已经取出来的那一批（可能为 0）；之后继续取，直到队列空或达到本轮上限。
    // 上限防止任务不断再投递任务而饿死 IO；剩下的留给下一轮——下一轮睡前复查会取到它们，不会睡着。
    for (int round = 0;; ++round)
    {
        for (std::size_t i = 0; i < n; ++i)
        {
            Task t = std::move(batch_[i]);
            RunTask(t);
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

    // lag 量的是真实耗时，所以不用可注入的假时钟。
    const auto busy_start = Clock::now();
    if (idle_active_ > 0)
        cached_now_ = Now();

    for (int i = 0; i < n; ++i)
        static_cast<Pollable*>(events[i].data.ptr)->OnEvents(events[i].events);

    RunTasks(pending);
    RunTimers();
    ProcessClosing();

    const auto lag = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - busy_start).count());
    if (lag > stats_.max_lag_ns.load(std::memory_order_relaxed))
        stats_.max_lag_ns.store(lag, std::memory_order_relaxed);
}

void EventLoop::Run()
{
    while (!quit_)
        RunOnce(-1);
}

} // namespace breeze
