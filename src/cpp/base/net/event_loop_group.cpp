#include "event_loop_group.h"

#include <cerrno>
#include <thread>

#include "connection_handle.h"
#include "event_loop.h"
#include "group_access.h"
#include "group_registry.h"
#include "logger.h"
#include "logger_macros.h"
#include "thread.h"

namespace breeze
{

struct EventLoopGroup::Worker
{
    EventLoop loop;
    Thread thread;
};

EventLoopGroup::EventLoopGroup(EventLoopGroupOptions options)
    : options_(options)
{
}

// 析构只做兜底：没 Shutdown 过就直接停，不排空，也就不会回调 OnClosed（那时 handler 可能已经销毁）。
EventLoopGroup::~EventLoopGroup()
{
    if (state_ != State::Running)
        return;
    state_ = State::Stopped;
    StopWorkers(workers_.size());
    GroupRegistry::Global().Unregister(id_);
}

int EventLoopGroup::Start()
{
    if (state_ != State::Idle)
        LOG_FATAL("EventLoopGroup::Start 只能在 Idle 状态调用一次（Shutdown 之后也不能再 Start）");
    if (options_.io_threads < 1 || options_.io_threads > ConnectionHandle::kMaxLoops ||
        options_.max_connections_per_loop < 1 || options_.max_connections_per_loop > ConnectionHandle::kMaxSlots)
        return EINVAL;

    std::uint32_t id = 0;
    if (const int e = GroupRegistry::Global().Register(this, id); e != 0)
        return e;
    id_ = id;

    // 先把所有 loop 建好，再起线程：中途失败时只有"已起的线程"需要 join。
    for (std::uint32_t i = 0; i < options_.io_threads; ++i)
    {
        auto w = std::make_unique<Worker>();
        const LoopOptions lo{id, i, options_.max_connections_per_loop};
        if (const int e = w->loop.Init(lo); e != 0)
        {
            workers_.clear();
            GroupRegistry::Global().Unregister(id);
            return e;
        }
        workers_.push_back(std::move(w));
    }
    for (std::size_t i = 0; i < workers_.size(); ++i)
    {
        Worker* w = workers_[i].get();
        if (const int e = w->thread.Start([w] { w->loop.Run(); }, "io"); e != 0)
        {
            StopWorkers(i);
            workers_.clear();
            GroupRegistry::Global().Unregister(id);
            return e;
        }
    }
    state_ = State::Running;
    return 0;
}

// 停掉前 started 个已经起了线程的 worker。
void EventLoopGroup::StopWorkers(std::size_t started)
{
    for (std::size_t i = 0; i < started; ++i)
        workers_[i]->loop.Quit();
    for (std::size_t i = 0; i < started; ++i)
        workers_[i]->thread.Join();
}

namespace
{
// 强关之后等 OnClosed 跑完的时间上限：只有 handler 阻塞在回调里才会用满。
constexpr std::chrono::milliseconds kForceCloseWait{2000};
} // namespace

bool EventLoopGroup::WaitDrained(std::chrono::steady_clock::time_point until,
                                 std::move_only_function<bool()>& idle) const
{
    for (;;)
    {
        // 还要求每个 loop 的 BeginShutdown 都跑完：否则连接表暂时为空只是因为 loop 还没处理到停机任务，
        // 它的 Acceptor 还在接新连接，这时 Quit 出去就会让投给已退出 loop 的连接没人服务。
        bool drained = true;
        for (const auto& w : workers_)
            drained = drained && w->loop.shutdown_started() && w->loop.table().Live() == 0;
        if (drained)
            return true;
        if (std::chrono::steady_clock::now() >= until)
            return false;
        if (!(idle && idle()))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void EventLoopGroup::DrainConnections(std::chrono::milliseconds deadline, std::move_only_function<bool()>& idle)
{
    const auto limit = std::chrono::steady_clock::now() + deadline;
    // 各 loop 先停自己的 Acceptor / Connector，再对连接优雅关闭，此后的新连接任务一律丢弃。
    for (auto& w : workers_)
    {
        EventLoop* l = &w->loop;
        l->Post([l, deadline] { l->BeginShutdown(deadline); });
    }
    if (WaitDrained(limit, idle))
        return;
    LOG_WARN("EventLoopGroup 停机：{}ms 内连接没有走完，强关剩余连接", deadline.count());
    for (auto& w : workers_)
    {
        EventLoop* l = &w->loop;
        l->Post([l] { l->CloseAllConnections(); });
    }
    if (!WaitDrained(std::chrono::steady_clock::now() + kForceCloseWait, idle))
        LOG_ERROR("EventLoopGroup 停机：强关之后连接仍未清空，直接停 loop");
}

void EventLoopGroup::Shutdown(std::chrono::milliseconds deadline, std::move_only_function<bool()> idle)
{
    if (state_ != State::Running)
        return;
    state_ = State::Stopping;
    DrainConnections(deadline, idle);
    StopWorkers(workers_.size());
    GroupRegistry::Global().Unregister(id_); // 之后本 group 的句柄解析不到
    state_ = State::Stopped;
    // loop 对象留到析构：引用它们的 NetServer / 迟到的任务不会碰到已销毁的对象，线程已经 join，不会再有人运行它们。
}

NetStats EventLoopGroup::GetStats() const
{
    NetStats out;
    for (const auto& w : workers_)
    {
        out.connections += w->loop.table().Live();
        w->loop.stats().AddTo(out);
    }
    return out;
}

// ---- GroupAccess ----

std::size_t GroupAccess::LoopCount(const EventLoopGroup& g) noexcept { return g.workers_.size(); }

EventLoop* GroupAccess::TryLoopAt(const EventLoopGroup& g, std::size_t i) noexcept
{
    return i < g.workers_.size() ? &g.workers_[i]->loop : nullptr;
}

EventLoop& GroupAccess::LoopAt(const EventLoopGroup& g, std::size_t i)
{
    EventLoop* l = TryLoopAt(g, i);
    if (l == nullptr)
        LOG_FATAL("EventLoopGroup 的 loop 下标越界 i={} n={}", i, g.workers_.size());
    return *l;
}

EventLoop& GroupAccess::PickLoop(const EventLoopGroup& g)
{
    // 只在从未 Start 过时是编程错误：Shutdown 期间（Stopping）accept 线程里仍可能走到这里，选到的 loop 会拒绝新连接。
    if (g.workers_.empty())
        LOG_FATAL("EventLoopGroup 尚未 Start，不能选 loop");
    EventLoop* best = &g.workers_[0]->loop;
    std::size_t best_live = best->ConnectionCount();
    for (std::size_t i = 1; i < g.workers_.size(); ++i)
    {
        EventLoop& l = g.workers_[i]->loop;
        if (const std::size_t live = l.ConnectionCount(); live < best_live)
        {
            best = &l;
            best_live = live;
        }
    }
    return *best;
}

} // namespace breeze
