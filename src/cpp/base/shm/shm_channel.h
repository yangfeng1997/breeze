// 一条共享内存通道：一个 memfd（两条单向环 + 控制块）加两个 eventfd。设计见 docs/共享内存IPC设计.md §2 §3 §6。
//
// 角色：Agent 创建（Create），App 经 UDS 拿到三个 fd 后接手（Attach）。数据方向：
//   Tx：App → Agent（App 是生产者，Agent 是消费者）
//   Rx：Agent → App
// 本类不管握手、不管 UDS、不管保留与重连（那是上层 ShmChannelServer / ShmChannelClient 的事），只负责：
// 创建并封印 memfd、映射、校验对端交来的 fd、按角色给出生产者与消费者。
//
// 对端交来的 fd 不可信（§6.5、§7）：Attach 会校验封印、大小、头部、布局、容量范围与两条环的游标，
// 任何一项不过就返回错误，不尝试修复，也不会留下半个通道。
//
// 错误码：EPROTO = 魔数、版本、头大小、页大小、对齐值不符（旧布局，应当销毁重建）；其余校验不过一律 EINVAL。
//
// 环对象（RingProducer / RingConsumer）里放着指向本通道映射的指针：**通道必须比它们活得久**。
// 通道可以移动（映射地址不变），环对象在移动之后仍然有效。

#pragma once

#include <cstdint>
#include <expected>

#include "base/shm/shm_defs.h"
#include "base/shm/shm_fd.h"
#include "base/shm/shm_mirror_map.h"
#include "base/shm/shm_ring.h"

namespace breeze::shm
{

struct ChannelOptions
{
    // 0 = 默认（容量 16 MiB；载荷上限 min(1 MiB, 容量 / 16)）。必须是 2 的幂且在 [1 MiB, max_ring_capacity]，
    // 否则 Create 返回 EINVAL：不静默取整，不静默裁剪。
    std::uint64_t tx_capacity = 0;
    std::uint64_t rx_capacity = 0;
    std::uint32_t tx_max_payload = 0;
    std::uint32_t rx_max_payload = 0;
    std::uint64_t max_ring_capacity = kDefaultMaxRingCapacity; // Agent 的单环配额
};

struct AttachLimits
{
    std::uint64_t max_ring_capacity = kDefaultMaxRingCapacity;
};

class ShmChannel
{
public:
    ShmChannel() = default;
    ShmChannel(ShmChannel&&) noexcept = default;
    ShmChannel& operator=(ShmChannel&&) noexcept = default;
    ShmChannel(const ShmChannel&) = delete;
    ShmChannel& operator=(const ShmChannel&) = delete;

    // 新建一条通道（Agent 一侧）：创建 memfd，初始化头与控制块，加封印，再创建两个 eventfd。
    static std::expected<ShmChannel, int> Create(const ChannelOptions& options = {}, const char* name = "breeze-shm");

    // 接手一条已有的通道（App 收到 Agent 发的三个 fd；或 Agent 重启后收到 App 传回来的三个 fd，即 Reattach）。
    static std::expected<ShmChannel, int> Attach(Fd memfd,
                                                 Fd tx_eventfd,
                                                 Fd rx_eventfd,
                                                 const AttachLimits& limits = {});

    bool valid() const noexcept { return memfd_.valid(); }
    const ChannelLayout& layout() const noexcept { return layout_; }

    // 要经 UDS 传出去的三个 fd（调用方 dup 或经 SCM_RIGHTS 发送，所有权仍在本对象）。
    int memfd() const noexcept { return memfd_.get(); }
    int tx_eventfd() const noexcept { return tx_efd_.get(); }
    int rx_eventfd() const noexcept { return rx_efd_.get(); }

    // 共享内存里的头与控制块。头里除 epoch 以外的字段创建之后只读（对端也能写，所以接手时只信校验过的本地拷贝）。
    ChannelHeader& header() const noexcept { return *header_; }
    RingControl& tx_control() const noexcept { return *tx_ctl_; }
    RingControl& rx_control() const noexcept { return *rx_ctl_; }

    // 按角色取环的一端。每条环同一时刻只能有一个生产者对象、一个消费者对象。
    RingProducer TxProducer() const { return RingProducer(TxView()); } // App
    RingConsumer TxConsumer() const { return RingConsumer(TxView()); } // Agent
    RingProducer RxProducer() const { return RingProducer(RxView()); } // Agent
    RingConsumer RxConsumer() const { return RingConsumer(RxView()); } // App

    // 挂载时调用，返回加一之后的值。
    std::uint64_t BumpAppEpoch() const noexcept;
    std::uint64_t BumpAgentEpoch() const noexcept;

private:
    RingView TxView() const noexcept;
    RingView RxView() const noexcept;
    // 校验过的映射之后，把各个指针指向映射内部。
    void BindPointers() noexcept;

    Fd memfd_;
    Fd tx_efd_;
    Fd rx_efd_;
    SharedMapping control_; // 头 + 两个控制块，3 页
    MirrorMapping tx_data_;
    MirrorMapping rx_data_;
    ChannelLayout layout_;
    ChannelHeader* header_ = nullptr;
    RingControl* tx_ctl_ = nullptr;
    RingControl* rx_ctl_ = nullptr;
};

} // namespace breeze::shm
