#include "base/shm/shm_channel.h"

#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <new>
#include <utility>

#include "base/shm/shm_memfd.h"

namespace breeze::shm
{

namespace
{

std::uint32_t PageSize() noexcept { return static_cast<std::uint32_t>(::sysconf(_SC_PAGESIZE)); }

std::int64_t Diff(std::uint64_t a, std::uint64_t b) noexcept { return static_cast<std::int64_t>(a - b); }

// 接手时校验一条环的游标与 inflight 标记。此时对端可能还活着、还在推进游标（比如 App 重启而 Agent 一直在跑），
// 所以只检查"合法状态下永远成立"的关系，读的顺序也是为此选的：
// 每个比较里"较晚读到的"一侧在合法状态下只会更大，不会因为对端恰好在两次读之间前进而误判。
//   inflight 先读，w1 次之，r 再次，w2 最后：
//   - in <= w1：标记只会打在已经发布的记录上，in 读在 w1 之前；
//   - w1 - r <= capacity：r 读在 w1 之后，只增不减；
//   - r <= w2：w2 读在 r 之后，r 永远追不过 w。
// 对端在两次读之间倒退 write_idx 不在这里查：那需要对端恰好在两次加载之间篡改，没法确定性地测试，
// 而且消费者的 Peek 对 write_idx 倒退有同样的防线。
// inflight 与 read_idx 的精确关系（== 表示死在处理中）留给消费者角色的 RecoverFromCrash 查，那时旧消费者已不在。
bool RingStateValid(const RingControl& c, std::uint64_t capacity) noexcept
{
    constexpr std::uint64_t kAlignMask = kRecordAlign - 1;
    const std::uint64_t in = c.inflight_idx.load(std::memory_order_acquire);
    const std::uint64_t w1 = c.write_idx.load(std::memory_order_acquire);
    const std::uint64_t r = c.read_idx.load(std::memory_order_acquire);
    const std::uint64_t w2 = c.write_idx.load(std::memory_order_acquire);
    if (((w1 | r | w2) & kAlignMask) != 0)
        return false;
    if (Diff(w2, r) < 0 || Diff(w1, r) > static_cast<std::int64_t>(capacity))
        return false;
    if (in != kNoInflight && ((in & kAlignMask) != 0 || Diff(in, w1) > 0))
        return false;
    return c.consumer_sleeping.load(std::memory_order_acquire) <= 1;
}

} // namespace

void ShmChannel::BindPointers() noexcept
{
    std::byte* base = control_.data();
    header_ = reinterpret_cast<ChannelHeader*>(base);
    tx_ctl_ = reinterpret_cast<RingControl*>(base + layout_.tx.control_offset);
    rx_ctl_ = reinterpret_cast<RingControl*>(base + layout_.rx.control_offset);
}

RingView ShmChannel::TxView() const noexcept
{
    return RingView{tx_ctl_, tx_data_.data(), layout_.tx.capacity, layout_.tx.max_payload, tx_efd_.get()};
}

RingView ShmChannel::RxView() const noexcept
{
    return RingView{rx_ctl_, rx_data_.data(), layout_.rx.capacity, layout_.rx.max_payload, rx_efd_.get()};
}

std::uint64_t ShmChannel::BumpAppEpoch() const noexcept
{
    return header_->app_epoch.fetch_add(1, std::memory_order_acq_rel) + 1;
}

std::uint64_t ShmChannel::BumpAgentEpoch() const noexcept
{
    return header_->agent_epoch.fetch_add(1, std::memory_order_acq_rel) + 1;
}

std::expected<ShmChannel, int> ShmChannel::Create(const ChannelOptions& options, const char* name)
{
    const std::uint64_t tx_cap = options.tx_capacity != 0 ? options.tx_capacity : kDefaultRingCapacity;
    const std::uint64_t rx_cap = options.rx_capacity != 0 ? options.rx_capacity : kDefaultRingCapacity;
    const std::uint32_t tx_mp = options.tx_max_payload != 0 ? options.tx_max_payload : DefaultMaxPayload(tx_cap);
    const std::uint32_t rx_mp = options.rx_max_payload != 0 ? options.rx_max_payload : DefaultMaxPayload(rx_cap);

    auto layout = ComputeLayout(tx_cap, rx_cap, tx_mp, rx_mp, PageSize(), options.max_ring_capacity);
    if (!layout)
        return std::unexpected(layout.error());

    ShmChannel ch;
    ch.layout_ = *layout;

    auto memfd = CreateMemFd(name, layout->total_size);
    if (!memfd)
        return std::unexpected(memfd.error());
    ch.memfd_ = std::move(*memfd);

    auto control = SharedMapping::Map(ch.memfd_.get(), 0, 3ULL * layout->page_size);
    if (!control)
        return std::unexpected(control.error());
    ch.control_ = std::move(*control);
    auto tx_data = MirrorMapping::Map(ch.memfd_.get(), layout->tx.data_offset, layout->tx.capacity);
    if (!tx_data)
        return std::unexpected(tx_data.error());
    ch.tx_data_ = std::move(*tx_data);
    auto rx_data = MirrorMapping::Map(ch.memfd_.get(), layout->rx.data_offset, layout->rx.capacity);
    if (!rx_data)
        return std::unexpected(rx_data.error());
    ch.rx_data_ = std::move(*rx_data);

    auto tx_efd = CreateEventFd();
    if (!tx_efd)
        return std::unexpected(tx_efd.error());
    ch.tx_efd_ = std::move(*tx_efd);
    auto rx_efd = CreateEventFd();
    if (!rx_efd)
        return std::unexpected(rx_efd.error());
    ch.rx_efd_ = std::move(*rx_efd);

    // 初始化头与控制块（memfd 本来就是全零，这里显式构造并写上非零的初值）。
    std::byte* base = ch.control_.data();
    auto* header = new (base) ChannelHeader;
    header->fixed = ChannelHeaderFixed{
        .magic = kMagic,
        .layout_version = kLayoutVersion,
        .header_size = sizeof(ChannelHeader),
        .page_size = layout->page_size,
        .record_align = kRecordAlign,
        .flags = 0,
        .reserved = 0,
        .total_size = layout->total_size,
        .tx = {layout->tx.capacity, layout->tx.control_offset, layout->tx.data_offset, layout->tx.max_payload, 0},
        .rx = {layout->rx.capacity, layout->rx.control_offset, layout->rx.data_offset, layout->rx.max_payload, 0},
    };
    header->app_epoch.store(0, std::memory_order_relaxed);
    header->agent_epoch.store(0, std::memory_order_relaxed);
    for (const RingLayout* ring : {&layout->tx, &layout->rx})
    {
        auto* ctl = new (base + ring->control_offset) RingControl;
        ctl->write_idx.store(0, std::memory_order_relaxed);
        ctl->read_idx.store(0, std::memory_order_relaxed);
        ctl->consumer_sleeping.store(0, std::memory_order_relaxed);
        ctl->inflight_idx.store(kNoInflight, std::memory_order_relaxed);
    }
    ch.BindPointers();

    // 内容初始化完再封印：对端拿到 fd 之前大小就已经定死。
    if (const int err = SealMemFd(ch.memfd_.get()); err != 0)
        return std::unexpected(err);
    return ch;
}

std::expected<ShmChannel, int> ShmChannel::Attach(Fd memfd, Fd tx_eventfd, Fd rx_eventfd, const AttachLimits& limits)
{
    if (!memfd.valid() || !tx_eventfd.valid() || !rx_eventfd.valid())
        return std::unexpected(EINVAL);

    std::uint64_t file_size = 0;
    if (const int err = CheckSealedMemFd(memfd.get(), &file_size); err != 0)
        return std::unexpected(err);
    if (const int err = CheckEventFd(tx_eventfd.get()); err != 0)
        return std::unexpected(err);
    if (const int err = CheckEventFd(rx_eventfd.get()); err != 0)
        return std::unexpected(err);

    const std::uint32_t page = PageSize();
    // 文件至少要放得下三页控制块，且不超过配额允许的最大值：先挡住，免得下面映射一个离谱的大小。
    if (file_size < 3ULL * page || file_size > 3ULL * page + 2 * limits.max_ring_capacity)
        return std::unexpected(EINVAL);

    ShmChannel ch;
    auto control = SharedMapping::Map(memfd.get(), 0, 3ULL * page);
    if (!control)
        return std::unexpected(control.error());
    ch.control_ = std::move(*control);

    // 头整个拷到本地，校验之后只信这份拷贝（对端随时能改共享内存里的头）。
    ChannelHeaderFixed h;
    std::memcpy(&h, ch.control_.data(), sizeof(h));
    if (h.magic != kMagic || h.layout_version != kLayoutVersion || h.header_size != sizeof(ChannelHeader) ||
        h.page_size != page || h.record_align != kRecordAlign)
        return std::unexpected(EPROTO);
    if (h.flags != 0 || h.reserved != 0 || h.tx.reserved != 0 || h.rx.reserved != 0)
        return std::unexpected(EINVAL);

    // 偏移与总大小全部由容量重新算出来核对，对不上就是头被改过或根本不是我们的通道。
    auto layout =
        ComputeLayout(h.tx.capacity, h.rx.capacity, h.tx.max_payload, h.rx.max_payload, page, limits.max_ring_capacity);
    if (!layout)
        return std::unexpected(EINVAL);
    if (h.total_size != layout->total_size || file_size != layout->total_size ||
        h.tx.control_offset != layout->tx.control_offset || h.tx.data_offset != layout->tx.data_offset ||
        h.rx.control_offset != layout->rx.control_offset || h.rx.data_offset != layout->rx.data_offset)
        return std::unexpected(EINVAL);
    ch.layout_ = *layout;

    auto tx_data = MirrorMapping::Map(memfd.get(), layout->tx.data_offset, layout->tx.capacity);
    if (!tx_data)
        return std::unexpected(tx_data.error());
    ch.tx_data_ = std::move(*tx_data);
    auto rx_data = MirrorMapping::Map(memfd.get(), layout->rx.data_offset, layout->rx.capacity);
    if (!rx_data)
        return std::unexpected(rx_data.error());
    ch.rx_data_ = std::move(*rx_data);

    ch.memfd_ = std::move(memfd);
    ch.tx_efd_ = std::move(tx_eventfd);
    ch.rx_efd_ = std::move(rx_eventfd);
    ch.BindPointers();

    if (!RingStateValid(*ch.tx_ctl_, layout->tx.capacity) || !RingStateValid(*ch.rx_ctl_, layout->rx.capacity))
        return std::unexpected(EINVAL);
    return ch;
}

} // namespace breeze::shm
