#include "connection_table.h"

#include "connection.h"
#include "logger.h"
#include "logger_macros.h"

namespace breeze
{

ConnectionTable::ConnectionTable(std::uint32_t capacity)
    : capacity_(capacity)
    , slots_(std::make_unique<Slot[]>(capacity))
    , conns_(capacity)
    , free_(capacity)
    , free_count_(capacity)
{
    for (std::uint32_t i = 0; i < capacity; ++i)
        free_[i] = i;
}

// 在这里定义：销毁 conns_ 需要 Connection 是完整类型。没有走正常关闭流程就析构时，剩下的连接直接销毁
// （fd 随之关闭），不再回调 OnClosed。
ConnectionTable::~ConnectionTable() = default;

bool ConnectionTable::TryReserve() noexcept
{
    if (live_.fetch_add(1, std::memory_order_relaxed) >= capacity_)
    {
        live_.fetch_sub(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

ConnectionTable::Slot* ConnectionTable::Find(std::uint32_t slot, std::uint32_t generation) noexcept
{
    if (slot >= capacity_)
        return nullptr;
    Slot& s = slots_[slot];
    if (s.generation.load(std::memory_order_acquire) != generation ||
        s.state.load(std::memory_order_acquire) != State::Connected)
        return nullptr;
    return &s;
}

std::uint64_t ConnectionTable::PendingBytes(std::uint32_t slot) const noexcept
{
    return slot < capacity_ ? slots_[slot].pending_bytes.load(std::memory_order_relaxed) : 0;
}

std::uint32_t ConnectionTable::Allocate(std::uint64_t high_water)
{
    if (free_count_ == 0)
        LOG_FATAL("ConnectionTable 没有空闲槽：建连接之前必须先 TryReserve 成功");
    const std::uint32_t idx = free_[free_head_];
    free_head_ = (free_head_ + 1) % capacity_;
    --free_count_;

    Slot& s = slots_[idx];
    s.high_water.store(high_water, std::memory_order_relaxed);
    s.state.store(State::Connected, std::memory_order_release);
    return idx;
}

void ConnectionTable::Install(std::uint32_t slot, std::unique_ptr<Connection> conn) { conns_[slot] = std::move(conn); }

Connection* ConnectionTable::Current(std::uint32_t slot, std::uint32_t generation) noexcept
{
    if (slot >= capacity_ || slots_[slot].generation.load(std::memory_order_relaxed) != generation)
        return nullptr;
    return conns_[slot].get();
}

void ConnectionTable::Reclaim(std::uint32_t slot)
{
    Slot& s = slots_[slot];
    conns_[slot].reset();
    // 先让旧句柄失效，再清零：发送方只在 generation 相符时才会累加 pending_bytes。
    s.generation.fetch_add(1, std::memory_order_release);
    s.state.store(State::Free, std::memory_order_release);
    s.pending_bytes.store(0, std::memory_order_relaxed);

    free_[(free_head_ + free_count_) % capacity_] = slot;
    ++free_count_;
    live_.fetch_sub(1, std::memory_order_relaxed);
}

} // namespace breeze
