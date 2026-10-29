#include "connection_handle.h"

#include <cstdio>

#include "connection_handler.h"
#include "event_loop.h"
#include "group_access.h"
#include "group_registry.h"

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
        return SendResult::Closed;
    ConnectionTable::Slot* s = loop->table().Find(slot(), generation());
    if (s == nullptr)
        return SendResult::Closed;

    // 水位：先累加再比较（校验与累加不是原子的，见 ConnectionTable 头注释）。超限就把这次的份额退回去，
    // 本条消息不发，并让连接以 SlowConsumer 关闭。
    const std::uint64_t len = payload.size();
    const std::uint64_t pending = s->pending_bytes.fetch_add(len, std::memory_order_relaxed) + len;
    if (pending > s->high_water.load(std::memory_order_relaxed))
    {
        s->pending_bytes.fetch_sub(len, std::memory_order_relaxed);
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

} // namespace breeze
