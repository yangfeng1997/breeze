// 进程级的 group 注册表：固定 16 项的原子指针数组，ConnectionHandle 的 group 位就是下标。设计见 docs/网络库设计.md §4.3。
//
// EventLoopGroup::Start() 占第一个空位（使用方不指定编号），用满返回 ENOSPC；Shutdown / 析构末尾注销。
// Find 不持有任何引用：group 销毁与 Find 的并发由使用方保证（句柄文档里写明）。

#pragma once

#include <array>
#include <atomic>
#include <cstdint>

#include "base/net/connection/connection_handle.h"

namespace breeze
{

class EventLoopGroup;

class GroupRegistry
{
public:
    static constexpr std::uint32_t kCapacity = ConnectionHandle::kMaxGroups;

    // 进程唯一的实例。类本身可以实例化，测试用自己的实例验证"注册表满"而不占用进程级的名额。
    static GroupRegistry& Global();

    // 占第一个空位，写入编号。返回 errno：用满 → ENOSPC。
    [[nodiscard]] int Register(EventLoopGroup* group, std::uint32_t& id_out) noexcept;
    void Unregister(std::uint32_t id) noexcept;
    // 编号越界或该位为空返回空。任意线程可调。
    EventLoopGroup* Find(std::uint32_t id) const noexcept;

private:
    std::array<std::atomic<EventLoopGroup*>, kCapacity> groups_{};
};

} // namespace breeze
