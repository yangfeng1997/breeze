#include "group_registry.h"

#include <cerrno>

namespace breeze
{

GroupRegistry& GroupRegistry::Global()
{
    static GroupRegistry instance;
    return instance;
}

int GroupRegistry::Register(EventLoopGroup* group, std::uint32_t& id_out) noexcept
{
    for (std::uint32_t i = 0; i < kCapacity; ++i)
    {
        EventLoopGroup* expected = nullptr;
        if (groups_[i].compare_exchange_strong(expected, group, std::memory_order_acq_rel))
        {
            id_out = i;
            return 0;
        }
    }
    return ENOSPC;
}

void GroupRegistry::Unregister(std::uint32_t id) noexcept
{
    if (id < kCapacity)
        groups_[id].store(nullptr, std::memory_order_release);
}

EventLoopGroup* GroupRegistry::Find(std::uint32_t id) const noexcept
{
    return id < kCapacity ? groups_[id].load(std::memory_order_acquire) : nullptr;
}

} // namespace breeze
