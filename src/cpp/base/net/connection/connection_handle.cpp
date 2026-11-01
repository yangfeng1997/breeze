#include "base/net/connection/connection_handle.h"

#include <algorithm>
#include <cstdio>
#include <limits>

#include "base/net/connection/connection_handler_interface.h"
#include "base/net/loop/event_loop.h"
#include "base/net/loop/group_access.h"
#include "base/net/loop/group_registry.h"

namespace breeze
{

namespace
{

// group / loop 解析不到（group 已销毁、编号越界）返回空。
EventLoop* ResolveLoop(ConnectionHandle h)
{
    const EventLoopGroup* g = GroupRegistry::Global().Find(h.group());
    return g != nullptr ? GroupAccess::TryLoopAt(*g, h.loop()) : nullptr;
}

} // namespace

std::string ConnectionHandle::ToString() const
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "g%u.l%u#%u.%u", group(), loop(), slot(), generation());
    return buf;
}

SendResult ConnectionHandle::Send(SharedBuffer payload) const
{
    EventLoop* loop = ResolveLoop(*this);
    if (loop == nullptr)
        return SendResult::Closed; // group 已销毁：没有 loop 可记
    ConnectionTable::Slot* s = loop->table().Find(slot(), generation());
    if (s == nullptr)
    {
        LoopStats::Add(loop->stats().send_closed_stale);
        return SendResult::Closed;
    }

    // 水位：先累加再比较（校验与累加不是原子的，见 ConnectionTable 头注释）。超限就把这次的份额退回去，
    // 本条消息不发，并让连接以 SlowConsumer 关闭。
    const std::uint64_t len = payload.size();
    const std::uint64_t pending = s->pending_bytes.fetch_add(len, std::memory_order_relaxed) + len;
    if (pending > s->high_water.load(std::memory_order_relaxed))
    {
        s->pending_bytes.fetch_sub(len, std::memory_order_relaxed);
        LoopStats::Add(loop->stats().send_closed_slow_consumer);
        loop->PostOp(EventLoop::Op::Close, *this, static_cast<std::uint32_t>(CloseReason::SlowConsumer), {});
        return SendResult::Closed;
    }
    loop->PostOp(EventLoop::Op::Send, *this, 0, std::move(payload));
    return SendResult::Ok;
}

void ConnectionHandle::Close() const
{
    if (EventLoop* loop = ResolveLoop(*this))
        loop->PostOp(EventLoop::Op::Close, *this, static_cast<std::uint32_t>(CloseReason::LocalClose), {});
}

void ConnectionHandle::CloseGracefully(std::chrono::milliseconds timeout) const
{
    EventLoop* loop = ResolveLoop(*this);
    if (loop == nullptr)
        return;
    const auto ms = std::clamp<std::int64_t>(timeout.count(), 0, std::numeric_limits<std::uint32_t>::max());
    loop->PostOp(EventLoop::Op::CloseGracefully, *this, static_cast<std::uint32_t>(ms), {});
}

void ConnectionHandle::PauseRead() const
{
    if (EventLoop* loop = ResolveLoop(*this))
        loop->PostOp(EventLoop::Op::PauseRead, *this, 0, {});
}

void ConnectionHandle::ResumeRead() const
{
    if (EventLoop* loop = ResolveLoop(*this))
        loop->PostOp(EventLoop::Op::ResumeRead, *this, 0, {});
}

} // namespace breeze
