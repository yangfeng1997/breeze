// 共享内存通道的线格式：常量、记录头、通道头、环控制块，以及布局的计算与校验。
// 设计见 docs/共享内存IPC设计.md §3 §4。**对端能随时改这里的任何字节**（§7），所以读到的值一律先拷到本地再校验。
//
// 放进共享内存的原子变量必须无锁（下面有 static_assert），否则 std::atomic 会退化成进程内的锁，跨进程不起作用。
// 统计计数器每个只有一个写者（见 Counter），读者随便读。

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <type_traits>

namespace breeze::shm
{

inline constexpr std::uint64_t kMagic = 0x4252'5A53'484D'3031ULL; // "BRZSHM01"
inline constexpr std::uint32_t kLayoutVersion = 1;

// 记录起点按 64 字节（缓存行）对齐；头 16 字节。依据与代价见设计文档 §4。
inline constexpr std::uint32_t kRecordAlign = 64;
inline constexpr std::uint32_t kRecordHeaderSize = 16;

// 单环容量：2 的幂、[1 MiB, 上限]。2 的幂同时保证页对齐与缓存行对齐（页最大 64 KiB），不用再单独校验。
inline constexpr std::uint64_t kMinRingCapacity = 1ULL << 20;
inline constexpr std::uint64_t kDefaultRingCapacity = 16ULL << 20;
inline constexpr std::uint64_t kDefaultMaxRingCapacity = 256ULL << 20;
// 单条载荷上限：默认 1 MiB，且不超过环容量的 1/16（保证一条记录占的空间远小于整环）。
inline constexpr std::uint32_t kDefaultMaxPayload = 1U << 20;

// inflight_idx 的"没有在处理"哨兵。不能用 0：0 会与 read_idx == 0 撞上，让接手的新进程误跳过第一条。
inline constexpr std::uint64_t kNoInflight = ~0ULL;

static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

// 记录占用的字节数：16 字节头加载荷，向上取整到 64。
constexpr std::uint64_t RecordSize(std::uint32_t payload_len) noexcept
{
    return (kRecordHeaderSize + static_cast<std::uint64_t>(payload_len) + kRecordAlign - 1) &
           ~static_cast<std::uint64_t>(kRecordAlign - 1);
}

// 环里的记录头。对端写，读者必须先拷到本地再校验。
struct RecordHeader
{
    std::uint32_t len;           // 载荷字节数
    std::uint16_t flags;         // 保留，必须为 0
    std::uint16_t reserved;      // 保留，必须为 0
    std::uint64_t enqueue_ts_ns; // 生产者 Commit 批次的单调时钟时刻（每条都有，0 无效）；见设计文档 §4"过期怎么算"
};
static_assert(sizeof(RecordHeader) == kRecordHeaderSize);
static_assert(std::is_trivially_copyable_v<RecordHeader>);

// 单写者的计数器：读-改-写用普通的 relaxed 读和写，不用 fetch_add（没有 lock 前缀）。
// 只能由对应角色的那一个线程写；读者（任何进程）读到的可能略旧，没关系。
class Counter
{
public:
    void Add(std::uint64_t n = 1) noexcept
    {
        v_.store(v_.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
    }
    std::uint64_t Get() const noexcept { return v_.load(std::memory_order_relaxed); }

private:
    std::atomic<std::uint64_t> v_{0};
};

struct ProducerStats
{
    Counter records_committed;
    Counter bytes_committed; // 载荷字节数，不含头与对齐
    Counter full_events;     // TryReserve 遇到 Full 的次数
    Counter wakeups_sent;    // 敲 eventfd 的次数
};

struct ConsumerStats
{
    Counter records_popped;
    Counter bytes_popped;
    Counter wakeups_received; // 醒来时 eventfd 里确实有计数
    Counter spurious_wakeups; // 醒来时 eventfd 是空的
    Counter inflight_skipped; // 接手时跳过的"处理到一半"的记录数（设计文档 §6.5）
};

// 每条环一页控制块。每个字段独占缓存行，免得两个角色互相拖慢（伪共享）。
struct RingControl
{
    alignas(64) std::atomic<std::uint64_t> write_idx;         // 只有生产者写；单调递增的字节计数，不回绕
    alignas(64) std::atomic<std::uint64_t> read_idx;          // 只有消费者写
    alignas(64) std::atomic<std::uint32_t> consumer_sleeping; // 消费者写 1、生产者 exchange 成 0；协议见设计文档 §5
    alignas(64) std::atomic<std::uint64_t> inflight_idx; // 只有消费者写：正在处理的记录的游标，kNoInflight 表示没有
    alignas(64) ProducerStats producer_stats;
    alignas(64) ConsumerStats consumer_stats;
};
static_assert(alignof(RingControl) == 64);

// 通道头里创建之后不再改变的部分。接手方把它整个拷到本地，校验之后只用本地的拷贝。
struct RingDesc
{
    std::uint64_t capacity;
    std::uint64_t control_offset;
    std::uint64_t data_offset;
    std::uint32_t max_payload;
    std::uint32_t reserved;
};

struct ChannelHeaderFixed
{
    std::uint64_t magic;
    std::uint32_t layout_version;
    std::uint32_t header_size; // sizeof(ChannelHeader)
    std::uint32_t page_size;
    std::uint32_t record_align;
    std::uint32_t flags; // 保留，必须为 0
    std::uint32_t reserved;
    std::uint64_t total_size;
    RingDesc tx; // App → Agent
    RingDesc rx; // Agent → App
};
static_assert(std::is_trivially_copyable_v<ChannelHeaderFixed>);

struct ChannelHeader
{
    ChannelHeaderFixed fixed;
    alignas(64) std::atomic<std::uint64_t> app_epoch;   // 每次 App 挂载加一
    alignas(64) std::atomic<std::uint64_t> agent_epoch; // 每次 Agent 挂载（含 Reattach）加一
};
static_assert(alignof(ChannelHeader) == 64);
// 头和每个控制块各占一页，页至少 4096（ComputeLayout 校验）。
static_assert(sizeof(ChannelHeader) <= 4096 && sizeof(RingControl) <= 4096);

// 本地算出来的布局（不在共享内存里）。
struct RingLayout
{
    std::uint64_t capacity = 0;
    std::uint64_t control_offset = 0;
    std::uint64_t data_offset = 0;
    std::uint32_t max_payload = 0;
};

struct ChannelLayout
{
    std::uint32_t page_size = 0;
    std::uint64_t total_size = 0;
    RingLayout tx;
    RingLayout rx;
};

constexpr bool IsPowerOfTwo(std::uint64_t v) noexcept { return v != 0 && (v & (v - 1)) == 0; }

// 容量合法：2 的幂且在 [kMinRingCapacity, max_capacity]。
constexpr bool IsValidCapacity(std::uint64_t capacity, std::uint64_t max_capacity) noexcept
{
    return IsPowerOfTwo(capacity) && capacity >= kMinRingCapacity && capacity <= max_capacity;
}

constexpr std::uint32_t DefaultMaxPayload(std::uint64_t capacity) noexcept
{
    const std::uint64_t limit = capacity / 16;
    return limit < kDefaultMaxPayload ? static_cast<std::uint32_t>(limit) : kDefaultMaxPayload;
}

constexpr bool IsValidMaxPayload(std::uint32_t max_payload, std::uint64_t capacity) noexcept
{
    return max_payload >= 1 && static_cast<std::uint64_t>(max_payload) <= capacity / 16;
}

// 按已经定好的参数算布局：页对齐的 [头 | Tx 控制块 | Rx 控制块 | Tx 数据 | Rx 数据]。参数全部要合法，不做取整或裁剪。
// 失败返回 EINVAL。创建方与接手方都走这一个函数，接手方据此核对共享内存头里的偏移（对端交来的东西不可信）。
std::expected<ChannelLayout, int> ComputeLayout(std::uint64_t tx_capacity,
                                                std::uint64_t rx_capacity,
                                                std::uint32_t tx_max_payload,
                                                std::uint32_t rx_max_payload,
                                                std::uint32_t page_size,
                                                std::uint64_t max_ring_capacity);

} // namespace breeze::shm
