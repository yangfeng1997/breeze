#include "timer_wheel.h"

#include <algorithm>
#include <bit>
#include <limits>

namespace breeze
{

namespace
{

constexpr std::int64_t kSlotMask = TimerWheel::kSlots - 1;
constexpr std::int64_t kMaxTick = std::int64_t{1} << 61;

} // namespace

TimerWheel::TimerWheel(std::int64_t tick_ms)
    : tick_ms_(tick_ms >= 1 ? tick_ms : 1)
{
    for (auto& level : head_)
    {
        for (auto& h : level)
            h = kNil;
    }
}

TimerWheel::~TimerWheel() = default;

std::int64_t TimerWheel::ToTicks(std::int64_t ms) const noexcept
{
    if (ms <= 0)
        return 1;
    std::int64_t ticks = ms / tick_ms_ + (ms % tick_ms_ != 0 ? 1 : 0); // 向上取整，不会溢出
    return std::min(ticks, kMaxTicks);
}

std::uint32_t TimerWheel::AllocNode()
{
    if (free_head_ != kNil)
    {
        const std::uint32_t idx = free_head_;
        free_head_ = At(idx).next;
        return idx;
    }
    if (node_count_ >= std::numeric_limits<std::uint32_t>::max() - 1) // 句柄里下标占 32 位，且 0 保留
        return kNil;
    if ((node_count_ & (kChunkSize - 1)) == 0)
        chunks_.push_back(std::make_unique<Node[]>(kChunkSize));
    return node_count_++;
}

void TimerWheel::FreeNode(std::uint32_t idx)
{
    Node& n = At(idx);
    n.state = State::kFree;
    n.bucket = kNoBucket;
    n.stop_repeat = false;
    if (++n.gen == 0)
        n.gen = 1;
    n.next = free_head_;
    free_head_ = idx;
    // 节点已经回到空闲链、状态一致之后才销毁回调：捕获物的析构万一碰到时间轮，看到的也是一致的。
    Callback dead = std::move(n.cb);
    n.cb = nullptr;
}

std::uint32_t TimerWheel::Resolve(TimerId id) const
{
    const std::uint64_t low = id & 0xffffffffu;
    if (low == 0 || low - 1 >= node_count_)
        return kNil;
    const auto idx = static_cast<std::uint32_t>(low - 1);
    const Node& n = At(idx);
    if (n.state == State::kFree || n.gen != static_cast<std::uint32_t>(id >> 32))
        return kNil;
    return idx;
}

TimerWheel::TimerId TimerWheel::AddNode(std::int64_t delay_ms, bool repeating, Callback cb, Repeat policy)
{
    if (!cb) // 空回调触发时会抛 bad_function_call；在这里拒绝
        return kInvalidTimerId;
    const std::uint32_t idx = AllocNode();
    if (idx == kNil)
        return kInvalidTimerId;
    Node& n = At(idx);
    const std::int64_t ticks = ToTicks(delay_ms);
    n.cb = std::move(cb);
    n.interval = repeating ? ticks : 0;
    n.repeat = policy;
    n.stop_repeat = false;
    n.expire = cur_tick_ + ticks;
    n.seq = next_seq_++;
    ++size_;
    Schedule(idx);
    return MakeId(idx);
}

TimerWheel::TimerId TimerWheel::AddTimer(std::int64_t delay_ms, Callback cb)
{
    return AddNode(delay_ms, false, std::move(cb), Repeat::kSkipMissed);
}

TimerWheel::TimerId TimerWheel::AddRepeating(std::int64_t interval_ms, Callback cb, Repeat policy)
{
    return AddNode(interval_ms, true, std::move(cb), policy);
}

// ---- 槽链表与位图 ----

void TimerWheel::LinkTo(std::uint32_t idx, int level, int slot)
{
    Node& n = At(idx);
    n.state = State::kPending;
    n.bucket = static_cast<std::uint16_t>(level * kSlots + slot);
    n.prev = kNil;
    n.next = head_[level][slot];
    if (n.next != kNil)
        At(n.next).prev = idx;
    head_[level][slot] = idx;
    occupied_[level][slot >> 6] |= std::uint64_t{1} << (slot & 63);
}

void TimerWheel::Unlink(std::uint32_t idx)
{
    Node& n = At(idx);
    const int level = n.bucket / kSlots;
    const int slot = n.bucket % kSlots;
    if (n.prev != kNil)
        At(n.prev).next = n.next;
    else
        head_[level][slot] = n.next;
    if (n.next != kNil)
        At(n.next).prev = n.prev;
    if (head_[level][slot] == kNil)
        occupied_[level][slot >> 6] &= ~(std::uint64_t{1} << (slot & 63));
    n.bucket = kNoBucket;
}

// 按距离当前 tick 的远近选层与槽。第 L 层每个槽覆盖 256^L 个 tick，槽号取到期 tick 的第 L 个字节。
// 同一层的差值 diff < 256^(L+1) 保证到期 tick 与当前 tick 在该层只差 1..256 个槽，所以槽号不会和"已经轮过的"混淆。
void TimerWheel::Schedule(std::uint32_t idx)
{
    Node& n = At(idx);
    const std::int64_t diff = std::max<std::int64_t>(n.expire - cur_tick_, 0);
    int level;
    int slot;
    if (diff < (std::int64_t{1} << 8))
    {
        // diff == 0：降级时恰好到期，放进当前 tick 的槽，紧接着就会被收走
        level = 0;
        slot = static_cast<int>((diff == 0 ? cur_tick_ : n.expire) & kSlotMask);
    }
    else if (diff < (std::int64_t{1} << 16))
    {
        level = 1;
        slot = static_cast<int>((n.expire >> 8) & kSlotMask);
    }
    else if (diff < (std::int64_t{1} << 24))
    {
        level = 2;
        slot = static_cast<int>((n.expire >> 16) & kSlotMask);
    }
    else if (diff < (std::int64_t{1} << 32))
    {
        level = 3;
        slot = static_cast<int>((n.expire >> 24) & kSlotMask);
    }
    else
    {
        // 超出四层能表示的范围：放进最高层"最晚才会轮到"的那个槽，降级时发现还没到期会再排一次（每约 49 天一跳，tick =
        // 1ms 时）
        level = 3;
        slot = static_cast<int>(((cur_tick_ >> 24) + kSlotMask) & kSlotMask);
    }
    LinkTo(idx, level, slot);
}

void TimerWheel::Cascade(int level, int slot)
{
    // 整条链先摘下来再逐个重排：重排可能落回同一层，不能边遍历边往原链表里插
    std::uint32_t idx = head_[level][slot];
    head_[level][slot] = kNil;
    occupied_[level][slot >> 6] &= ~(std::uint64_t{1} << (slot & 63));
    while (idx != kNil)
    {
        const std::uint32_t next = At(idx).next;
        Schedule(idx);
        idx = next;
    }
}

int TimerWheel::FirstOccupiedFrom(int level, int start) const
{
    const std::uint64_t* w = occupied_[level];
    const int wi = start >> 6;
    const int bi = start & 63;
    std::uint64_t m = w[wi] & (~std::uint64_t{0} << bi);
    if (m != 0)
        return wi * 64 + std::countr_zero(m) - start;
    for (int k = 1; k < kSlots / 64; ++k)
    {
        const int j = (wi + k) & (kSlots / 64 - 1);
        if (w[j] != 0)
            return (j * 64 + std::countr_zero(w[j]) - start + kSlots) & (kSlots - 1);
    }
    m = w[wi] & ((std::uint64_t{1} << bi) - 1); // 绕回来：start 所在字里低于 start 的那些位
    if (m != 0)
        return (wi * 64 + std::countr_zero(m) - start + kSlots) & (kSlots - 1);
    return -1;
}

// 第 L 层的槽在当前 tick 所在"组"的下一个组边界（256^L 的整数倍）被降级；第 0 层的槽就是在它自己的 tick 触发。
// 各层各自环形找出最近的非空槽，取最小的那个 tick。
std::int64_t TimerWheel::NextTick() const
{
    std::int64_t best = kNoTick;
    for (int level = 0; level < kLevels; ++level)
    {
        const int shift = kSlotBits * level;
        const std::int64_t group = cur_tick_ >> shift;
        const int d = FirstOccupiedFrom(level, static_cast<int>((group + 1) & kSlotMask));
        if (d < 0)
            continue;
        best = std::min(best, (group + 1 + d) << shift);
    }
    return best;
}

// ---- 触发 ----

void TimerWheel::ProcessTick(std::int64_t tick, std::size_t& fired)
{
    cur_tick_ = tick;
    // 逢 256^L 的整数倍 tick，把第 L 层当前这一格降到下面各层（降下来的条目到期 tick 与当前 tick 在该层只差 0..255 格，
    // 差 0 格的一定落进更低层，不会落回正在处理的这一格，所以各层之间没有先后依赖；习惯上从高到低）
    for (int level = kLevels - 1; level >= 1; --level)
    {
        const int shift = kSlotBits * level;
        if ((tick & ((std::int64_t{1} << shift) - 1)) == 0)
            Cascade(level, static_cast<int>((tick >> shift) & kSlotMask));
    }

    const int slot = static_cast<int>(tick & kSlotMask);
    std::uint32_t idx = head_[0][slot];
    if (idx == kNil)
        return;
    head_[0][slot] = kNil;
    occupied_[0][slot >> 6] &= ~(std::uint64_t{1} << (slot & 63));
    // 整个槽先摘下来按创建顺序排好再逐个触发：回调里 Cancel / Reset 其中某个，靠 (下标, 代数, 状态) 识别，不用碰链表。
    due_.clear();
    due_pos_ = 0;
    while (idx != kNil)
    {
        Node& n = At(idx);
        const std::uint32_t next = n.next;
        n.state = State::kDue;
        n.bucket = kNoBucket;
        due_.push_back({idx, n.gen, n.seq});
        idx = next;
    }
    std::sort(due_.begin(), due_.end(), [](const DueEntry& a, const DueEntry& b) { return a.seq < b.seq; });
    DrainDue(fired);
}

// 触发 due_ 里剩下的。回调抛异常时 due_pos_ 已经指向下一个，没触发的留着，下一次 Advance 从这里继续。
void TimerWheel::DrainDue(std::size_t& fired)
{
    while (due_pos_ < due_.size())
    {
        const DueEntry e = due_[due_pos_++];
        const Node& n = At(e.idx);
        if (n.gen != e.gen || n.state != State::kDue) // 在轮到之前被 Cancel / Reset 了
            continue;
        Fire(e.idx);
        ++fired;
    }
    due_.clear();
    due_pos_ = 0;
}

void TimerWheel::Fire(std::uint32_t idx)
{
    Node& n = At(idx);
    n.state = State::kFiring;
    if (n.interval == 0)
        --size_; // 一次性定时器一进入回调就不再算"还会触发"
    firing_ = idx;
    // 无论回调正常返回还是抛异常，都要收尾（重新排队或释放）
    struct Finisher
    {
        TimerWheel* w;
        std::uint32_t idx;
        ~Finisher() { w->Finish(idx); }
    } finisher{this, idx};
    n.cb();
}

void TimerWheel::Finish(std::uint32_t idx)
{
    firing_ = kNil;
    Node& n = At(idx);
    if (n.interval > 0 && !n.stop_repeat)
    {
        std::int64_t next = cur_tick_ + n.interval;
        // 丢掉错过的周期：跳到"仍与原相位对齐、且晚于本次推进目标"的第一个时刻
        if (n.repeat == Repeat::kSkipMissed && next <= advance_target_)
            next += n.interval * ((advance_target_ - next) / n.interval + 1);
        n.expire = next;
        n.seq = next_seq_++;
        Schedule(idx);
    }
    else
    {
        FreeNode(idx);
    }
}

std::size_t TimerWheel::Advance(std::int64_t now_ms)
{
    if (advancing_)
        return 0;
    if (!initialized_)
    {
        base_ms_ = now_ms;
        initialized_ = true;
        return 0;
    }
    if (now_ms < base_ms_)
        return 0;
    // 当前 tick 封顶在 kMaxTick：再大的 now 也不会让 tick 运算溢出（延迟最多 kMaxTicks，加起来仍在 int64 内）
    std::int64_t elapsed_ms = 0;
    if (__builtin_sub_overflow(now_ms, base_ms_, &elapsed_ms))
        elapsed_ms = std::numeric_limits<std::int64_t>::max();
    const std::int64_t target = std::min(elapsed_ms / tick_ms_, kMaxTick);

    advancing_ = true;
    advance_target_ = target;
    struct Leave
    {
        bool& flag;
        ~Leave() { flag = false; }
    } leave{advancing_};

    std::size_t fired = 0;
    DrainDue(fired);
    for (;;)
    {
        const std::int64_t next = NextTick();
        if (next > target)
        {
            if (target > cur_tick_)
                cur_tick_ = target; // 中间没有任何事件：一步跳过去
            break;
        }
        ProcessTick(next, fired);
    }
    return fired;
}

// ---- 取消 / 重置 / 查询 ----

bool TimerWheel::Cancel(TimerId id)
{
    const std::uint32_t idx = Resolve(id);
    if (idx == kNil)
        return false;
    Node& n = At(idx);
    switch (n.state)
    {
        case State::kPending:
            Unlink(idx);
            --size_;
            FreeNode(idx);
            return true;
        case State::kDue:
            --size_;
            FreeNode(idx);
            return true;
        case State::kFiring:
            if (n.interval > 0 && !n.stop_repeat)
            {
                n.stop_repeat = true; // 本次回调照常跑完，Finish 里释放
                --size_;
                return true;
            }
            return false;
        case State::kFree:
            break;
    }
    return false;
}

bool TimerWheel::Reset(TimerId id, std::int64_t delay_ms)
{
    const std::uint32_t idx = Resolve(id);
    if (idx == kNil)
        return false;
    Node& n = At(idx);
    if (n.state == State::kPending)
        Unlink(idx);
    else if (n.state != State::kDue) // kDue：本 tick 还没轮到它，改期即可，due_ 里那一项会因为状态变了被跳过
        return false;
    n.expire = cur_tick_ + ToTicks(delay_ms);
    n.seq = next_seq_++;
    Schedule(idx);
    return true;
}

bool TimerWheel::IsActive(TimerId id) const
{
    const std::uint32_t idx = Resolve(id);
    if (idx == kNil)
        return false;
    const Node& n = At(idx);
    switch (n.state)
    {
        case State::kPending:
        case State::kDue:
            return true;
        case State::kFiring:
            return n.interval > 0 && !n.stop_repeat;
        case State::kFree:
            break;
    }
    return false;
}

std::size_t TimerWheel::Clear()
{
    const std::size_t cancelled = size_;
    for (int level = 0; level < kLevels; ++level)
    {
        for (int slot = 0; slot < kSlots; ++slot)
        {
            while (head_[level][slot] != kNil)
            {
                const std::uint32_t idx = head_[level][slot];
                Unlink(idx);
                FreeNode(idx);
            }
        }
    }
    for (std::size_t i = due_pos_; i < due_.size(); ++i)
    {
        const Node& n = At(due_[i].idx);
        if (n.gen == due_[i].gen && n.state == State::kDue)
            FreeNode(due_[i].idx);
    }
    due_.clear();
    due_pos_ = 0;
    // 在回调里调用 Clear：正在跑的重复定时器也要停（一次性的本来就不再算数）
    if (firing_ != kNil && At(firing_).interval > 0)
        At(firing_).stop_repeat = true;
    size_ = 0;
    return cancelled;
}

std::optional<std::int64_t> TimerWheel::NextDeadlineMs() const
{
    if (!initialized_)
        return std::nullopt;
    std::int64_t tick = cur_tick_;
    if (due_pos_ >= due_.size())
    {
        tick = NextTick();
        if (tick == kNoTick)
            return std::nullopt;
    }
    std::int64_t ms = 0;
    if (__builtin_mul_overflow(tick, tick_ms_, &ms) || __builtin_add_overflow(ms, base_ms_, &ms))
        return std::numeric_limits<std::int64_t>::max();
    return ms;
}

} // namespace breeze
