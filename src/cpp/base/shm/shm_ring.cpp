#include "shm_ring.h"

#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>

#include "now.h"

namespace breeze::shm
{

namespace
{

constexpr std::uint64_t kAlignMask = kRecordAlign - 1;

// 单调时钟，与设计文档 §4"过期怎么算"一致（Now::SteadyNs）。0 是无效值，所以不返回 0。
// 放在 .cpp 里：只有每批的第一次预留才会调，不在热路径上，这样 breeze_time 不用暴露给使用方。
std::uint64_t NowNs() noexcept
{
    const auto t = static_cast<std::uint64_t>(Now::SteadyNs());
    return t == 0 ? 1 : t;
}

std::int64_t Diff(std::uint64_t a, std::uint64_t b) noexcept { return static_cast<std::int64_t>(a - b); }

// 接手角色时读两个游标：先 w 后 r。本角色的那一个游标不会再变（旧进程已经不在），对端的那一个可能还在推进，
// 但游标只增不减，所以对端在两次读之间推进，合法状态下下面的检查仍然成立：
// 都是 64 的倍数；r <= w（r 只可能在 w 之后读到更大的值，但永远追不过 w）；w - r <= capacity。
// 对端倒退游标这类情况不在这里查：生产者在重读 read_idx 时查，消费者用本地游标、不信 read_idx。
// 容量最大 256 MiB，转 int64 不会溢出。
bool ReadCursors(const RingControl& c, std::uint64_t capacity, std::uint64_t& w, std::uint64_t& r) noexcept
{
    w = c.write_idx.load(std::memory_order_acquire);
    r = c.read_idx.load(std::memory_order_acquire);
    if (((w | r) & kAlignMask) != 0)
        return false;
    const std::int64_t used = Diff(w, r);
    return used >= 0 && used <= static_cast<std::int64_t>(capacity);
}

// 敲 eventfd。EAGAIN（计数到顶）说明里面已经有一大堆未读的计数，消费者必然会醒，忽略即可。
void Notify(int fd) noexcept
{
    const std::uint64_t one = 1;
    ssize_t n;
    do
    {
        n = ::write(fd, &one, sizeof(one));
    } while (n < 0 && errno == EINTR);
}

} // namespace

// ---- 生产者 ----

RingProducer::RingProducer(const RingView& view)
    : v_(view)
    , mask_(view.capacity - 1)
{
    std::uint64_t w, r;
    if (!ReadCursors(*v_.control, v_.capacity, w, r))
    {
        corrupt_ = true;
        return;
    }
    w_ = published_ = w;
    cached_r_ = r;
}

Reserved RingProducer::TryReserve(std::uint32_t len) noexcept
{
    if (corrupt_)
        return {ReserveStatus::kCorrupt, {}};
    if (len > v_.max_payload)
        return {ReserveStatus::kTooLarge, {}};

    const std::uint64_t need = RecordSize(len);
    if (v_.capacity - (w_ - cached_r_) < need)
    {
        const std::uint64_t r = v_.control->read_idx.load(std::memory_order_acquire);
        // 消费者只能消费已发布的：r 不能倒退、不能超过 published_、必须对齐。
        if (Diff(r, cached_r_) < 0 || Diff(published_, r) < 0 || (r & kAlignMask) != 0)
        {
            corrupt_ = true;
            return {ReserveStatus::kCorrupt, {}};
        }
        cached_r_ = r;
        if (v_.capacity - (w_ - cached_r_) < need)
        {
            v_.control->producer_stats.full_events.Add();
            return {ReserveStatus::kFull, {}};
        }
    }

    if (batch_ts_ == 0)
        batch_ts_ = NowNs();
    std::byte* rec = v_.data + (w_ & mask_);
    const RecordHeader h{len, 0, 0, batch_ts_};
    std::memcpy(rec, &h, sizeof(h));
    w_ += need;
    ++pending_records_;
    pending_bytes_ += len;
    return {ReserveStatus::kOk, {rec + kRecordHeaderSize, len}};
}

void RingProducer::Commit() noexcept
{
    if (w_ == published_)
        return;
    RingControl& c = *v_.control;
    c.write_idx.store(w_, std::memory_order_release); // 记录写完之后才发布
    published_ = w_;
    c.producer_stats.records_committed.Add(pending_records_);
    c.producer_stats.bytes_committed.Add(pending_bytes_);
    pending_records_ = 0;
    pending_bytes_ = 0;
    batch_ts_ = 0;

    // Dekker：生产者"写 write_idx、栅栏、读 sleeping"，消费者"写 sleeping、栅栏、读 write_idx"，
    // 两边不会同时漏掉。只有一个生产者，exchange 防止重复敲 eventfd。
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (c.consumer_sleeping.load(std::memory_order_relaxed) != 0 &&
        c.consumer_sleeping.exchange(0, std::memory_order_acq_rel) != 0 && v_.eventfd >= 0)
    {
        Notify(v_.eventfd);
        c.producer_stats.wakeups_sent.Add();
    }
}

// ---- 消费者 ----

RingConsumer::RingConsumer(const RingView& view)
    : v_(view)
    , mask_(view.capacity - 1)
{
    std::uint64_t w, r;
    if (!ReadCursors(*v_.control, v_.capacity, w, r))
    {
        corrupt_ = true;
        return;
    }
    cursor_ = r;
    last_w_ = w;
}

PeekStatus RingConsumer::Peek(RecordView& out) noexcept
{
    if (corrupt_)
        return PeekStatus::kCorrupt;

    const std::uint64_t w = v_.control->write_idx.load(std::memory_order_acquire);
    if (Diff(w, last_w_) < 0) // write_idx 倒退
    {
        corrupt_ = true;
        return PeekStatus::kCorrupt;
    }
    if (w == cursor_)
        return PeekStatus::kEmpty;
    // 不变式 cursor_ <= last_w_ <= w（倒退已在上面排除），所以 w - cursor_ 一定是正数，只需防占用超过容量。
    // w 没对齐、可用字节不足一整条记录时，下面 RecordSize(h.len) > avail 一定成立（记录至少 64 字节）。
    const std::uint64_t avail = w - cursor_;
    if (avail > v_.capacity)
    {
        corrupt_ = true;
        return PeekStatus::kCorrupt;
    }
    last_w_ = w;

    // 先拷头再校验：不对共享内存里的字段做多次读取（防 TOCTOU）。
    RecordHeader h;
    std::memcpy(&h, v_.data + (cursor_ & mask_), sizeof(h));
    if (h.reserved != 0 || h.flags != 0 || h.len > v_.max_payload || RecordSize(h.len) > avail)
    {
        corrupt_ = true;
        return PeekStatus::kCorrupt;
    }

    out.payload = v_.data + (cursor_ & mask_) + kRecordHeaderSize;
    out.len = h.len;
    out.enqueue_ts_ns = h.enqueue_ts_ns;
    out.cursor = cursor_;
    cur_size_ = RecordSize(h.len);
    cur_len_ = h.len;
    return PeekStatus::kOk;
}

void RingConsumer::PopNoRelease() noexcept
{
    if (cur_size_ == 0)
        return;
    cursor_ += cur_size_;
    v_.control->consumer_stats.records_popped.Add();
    v_.control->consumer_stats.bytes_popped.Add(cur_len_);
    cur_size_ = 0;
    cur_len_ = 0;
}

void RingConsumer::Release() noexcept { v_.control->read_idx.store(cursor_, std::memory_order_release); }

void RingConsumer::Pop() noexcept
{
    if (cur_size_ == 0)
        return;
    PopNoRelease();
    Release();
}

void RingConsumer::MarkInflight(const RecordView& v) noexcept
{
    v_.control->inflight_idx.store(v.cursor, std::memory_order_release);
    // 进程被杀只看程序顺序：标记必须先于后面的处理（send、拷进别的环），编译器不能把它们挪到标记前面。
    std::atomic_signal_fence(std::memory_order_seq_cst);
}

void RingConsumer::ClearInflight() noexcept
{
    v_.control->inflight_idx.store(kNoInflight, std::memory_order_release);
    std::atomic_signal_fence(std::memory_order_seq_cst);
}

RecoverStatus RingConsumer::RecoverFromCrash() noexcept
{
    if (corrupt_)
        return RecoverStatus::kCorrupt;
    const std::uint64_t in = v_.control->inflight_idx.load(std::memory_order_acquire);
    if (in == kNoInflight)
        return RecoverStatus::kClean;
    const std::int64_t d = Diff(in, cursor_);
    if (d < 0) // 标记落后于游标：崩溃发生在两条记录之间
        return RecoverStatus::kClean;
    if (d > 0) // 标记超前于游标：不可能出现，页被写坏了
    {
        corrupt_ = true;
        return RecoverStatus::kCorrupt;
    }

    RecordView v;
    switch (Peek(v))
    {
        case PeekStatus::kEmpty: // 标记了但环里没有记录：没有东西可跳
            ClearInflight();
            return RecoverStatus::kClean;
        case PeekStatus::kCorrupt:
            return RecoverStatus::kCorrupt;
        case PeekStatus::kOk:
            break;
    }
    Pop();
    v_.control->consumer_stats.inflight_skipped.Add();
    ClearInflight();
    return RecoverStatus::kSkipped;
}

bool RingConsumer::PrepareToSleep() noexcept
{
    RingControl& c = *v_.control;
    c.consumer_sleeping.store(1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (c.write_idx.load(std::memory_order_acquire) != cursor_) // 回头再看一眼：这个窗口里有人写进来了
    {
        c.consumer_sleeping.store(0, std::memory_order_relaxed);
        return false;
    }
    return true;
}

void RingConsumer::WokeUp() noexcept
{
    RingControl& c = *v_.control;
    c.consumer_sleeping.store(0, std::memory_order_relaxed);
    if (v_.eventfd < 0)
        return;
    std::uint64_t counter;
    ssize_t n;
    do
    {
        n = ::read(v_.eventfd, &counter, sizeof(counter));
    } while (n < 0 && errno == EINTR);
    if (n == static_cast<ssize_t>(sizeof(counter)))
        c.consumer_stats.wakeups_received.Add();
    else
        c.consumer_stats.spurious_wakeups.Add();
}

} // namespace breeze::shm
