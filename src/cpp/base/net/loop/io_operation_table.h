#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace breeze::detail
{
// 仅loop线程使用。ID高32位为代次、低32位为槽号；保留0/1给非业务CQE。
// T的地址跨扩容稳定；调用方须等终结CQE及其回调结束后才能Remove。
template <class T, class Generation = std::uint32_t>
class IoOperationTable
{
    static_assert(std::is_unsigned_v<Generation> && sizeof(Generation) <= sizeof(std::uint32_t));
    struct Slot
    {
        std::unique_ptr<T> value;
        Generation generation = 1;
    };

public:
    std::uint64_t Insert(std::unique_ptr<T> value)
    {
        std::uint32_t index;
        if (free_.empty())
        {
            if (slots_.size() >= UINT32_MAX)
                std::abort();
            index = static_cast<std::uint32_t>(slots_.size());
            slots_.emplace_back();
        }
        else
        {
            index = free_.back();
            free_.pop_back();
        }
        Slot& slot = slots_[index];
        slot.value = std::move(value);
        ++live_;
        return (static_cast<std::uint64_t>(slot.generation) << 32) | index;
    }
    T* Get(std::uint64_t id) const noexcept
    {
        const auto index = static_cast<std::uint32_t>(id);
        if (index >= slots_.size() || slots_[index].generation != static_cast<std::uint32_t>(id >> 32))
            return nullptr;
        return slots_[index].value.get();
    }
    std::unique_ptr<T> Remove(std::uint64_t id)
    {
        if (Get(id) == nullptr)
            std::abort();
        const auto index = static_cast<std::uint32_t>(id);
        Slot& slot = slots_[index];
        auto value = std::move(slot.value);
        // 代次耗尽时永久退役该槽，不能回绕产生ABA，也不能因正常长期流量终止进程。
        if (slot.generation != std::numeric_limits<Generation>::max())
        {
            ++slot.generation;
            free_.push_back(index);
        }
        --live_;
        return value;
    }
    template <class Predicate>
    bool Any(Predicate predicate) const
    {
        for (const auto& slot : slots_)
            if (slot.value && predicate(*slot.value))
                return true;
        return false;
    }
    void Reserve(std::size_t n)
    {
        slots_.reserve(n);
        free_.reserve(n);
    }
    // 仅在内核已取消并清空CQ之后停机销毁；清空后不得重新提交请求。
    void Clear()
    {
        slots_.clear();
        free_.clear();
        live_ = 0;
    }
    bool empty() const noexcept { return live_ == 0; }

private:
    std::vector<Slot> slots_;
    std::vector<std::uint32_t> free_;
    std::size_t live_ = 0;
};
} // namespace breeze::detail
