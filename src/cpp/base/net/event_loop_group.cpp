#include "event_loop_group.h"

#include <cerrno>

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

EventLoopGroup::~EventLoopGroup() { Shutdown(std::chrono::milliseconds::zero()); }

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

void EventLoopGroup::Shutdown(std::chrono::milliseconds)
{
    if (state_ != State::Running)
        return;
    state_ = State::Stopped;
    StopWorkers(workers_.size());
    GroupRegistry::Global().Unregister(id_); // 之后本 group 的句柄解析不到
    // loop 对象留到析构：引用它们的 NetServer / 迟到的任务不会碰到已销毁的对象，线程已经 join，不会再有人运行它们。
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
    if (!g.running())
        LOG_FATAL("EventLoopGroup 尚未 Start 或已 Shutdown，不能选 loop");
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
