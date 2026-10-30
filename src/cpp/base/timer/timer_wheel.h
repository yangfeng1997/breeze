// 分层时间轮：O(1) 添加 / 取消 / 重置，推进时间按"有事件的 tick"走（空转的时间一步跳过），支持一次性与重复定时器。
//
//   TimerWheel wheel;                                    // tick 精度 1ms
//   auto id = wheel.AddTimer(500, [] { ... });           // 500ms 后触发一次
//   auto hb = wheel.AddRepeating(1000, [] { ... });      // 每 1000ms 一次
//   wheel.Reset(id, 500);                                // 把还没触发的定时器重新计时（连接空闲超时：每收到一个包就
//   Reset） wheel.Cancel(hb);
//   // 主循环里：
//   wheel.Advance(Now::SteadyMs());
//
// 时间基准：Advance 的参数必须是**单调**毫秒（Now::SteadyMs()），不要传墙钟 —— 系统时间被调整或 GM 拨时间会让一堆定时器
// 同时到期（或者长时间不到期）。第一次 Advance 只确定基准、不触发；now 回退会被忽略。
//
// 延迟是相对"当前时刻"的，当前时刻 = 最近一次 Advance 推进到的 tick（回调里 = 正在处理的那个 tick，所以回调里再
// AddTimer(50) 就是"本回调的预定时刻之后 50ms"，补触发时也保持相位）。两次 Advance 之间新加的定时器，计时起点是上一次
// Advance， 而不是调用 AddTimer 的那一刻：每帧一次 Advance 的循环里这就是"帧时间"，同一帧内加的定时器彼此一致。
//
// 同一个 tick 到期的定时器按**创建顺序**触发（重复定时器按它最近一次重新排队的顺序）。
//
// 重复定时器遇到长时间停顿（一帧卡了好几个周期）：默认 kSkipMissed 丢掉错过的周期、保持相位，只触发一次；
// kCatchUp 逐个补触发。回调慢的时候补触发会雪崩，所以默认是丢。
//
// 句柄：TimerId 带代数，定时器触发或取消后句柄失效，再对它 Cancel / Reset 是安全的空操作（返回 false），
// 不会误伤复用同一个槽位的新定时器。取消立刻释放回调（连同捕获的资源），不会拖到原本的到期时间。
//
// 回调里可以随意 AddTimer / Cancel / Reset（包括取消自己）；不能销毁时间轮、不能再调 Advance（会被忽略）。
// 回调抛异常：时间轮保持一致（本 tick 剩下到期的定时器留到下一次 Advance 触发，抛的那个按正常规则收尾），异常原样传给
// Advance 的调用方。 回调与其捕获物的析构函数里不要碰时间轮。
//
// 非线程安全：只在一个线程里用（通常是主循环线程）。

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace breeze
{

class TimerWheel
{
public:
    using TimerId = std::uint64_t;
    static constexpr TimerId kInvalidTimerId = 0;
    using Callback = std::move_only_function<void()>;

    enum class Repeat
    {
        kSkipMissed, // 错过的周期丢掉，下次触发仍对齐原来的相位
        kCatchUp,    // 错过的周期逐个补触发
    };

    // tick_ms：时间精度（毫秒）。小于 1 按 1。定时器最早在到期时间所在的那个 tick 触发，即最多晚 tick_ms - 1 毫秒。
    explicit TimerWheel(std::int64_t tick_ms = 1);
    // 没触发的定时器直接销毁、不触发。
    ~TimerWheel();

    TimerWheel(const TimerWheel&) = delete;
    TimerWheel& operator=(const TimerWheel&) = delete;

    // 推进到 now_ms（单调毫秒），按到期顺序触发回调，返回本次触发的次数。
    std::size_t Advance(std::int64_t now_ms);

    // delay_ms 后触发一次。delay_ms <= 0 按 1 个 tick 算（下一次 Advance 就会触发）。超过约 2^60 个 tick 的延迟按 2^60
    // 算。 返回的句柄用于 Cancel / Reset / IsActive。
    TimerId AddTimer(std::int64_t delay_ms, Callback cb);
    // 每隔 interval_ms 触发一次，首次在 interval_ms 之后。interval_ms <= 0 按 1 个 tick。
    TimerId AddRepeating(std::int64_t interval_ms, Callback cb, Repeat policy = Repeat::kSkipMissed);

    // 取消。返回 true = 阻止了这个定时器今后的触发；句柄已失效、一次性定时器已经在自己的回调里，返回 false。
    // 重复定时器可以在自己的回调里取消自己（返回 true，本次回调照常跑完，之后不再触发）。
    bool Cancel(TimerId id);

    // 把还没触发的定时器改成 delay_ms 之后触发（回调不动、不重新分配）。重复定时器只改下一次触发时间，周期不变。
    // 句柄失效或定时器正在自己的回调里返回 false。
    bool Reset(TimerId id, std::int64_t delay_ms);

    // 定时器还会触发吗（一次性：还没触发也没取消；重复：没取消）。
    bool IsActive(TimerId id) const;

    // 取消全部定时器（销毁回调，不触发）。返回取消的个数。
    std::size_t Clear();

    // 还会触发的定时器个数。
    std::size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }
    std::int64_t tick_ms() const noexcept { return tick_ms_; }

    // 下一次"有事可做"的时刻（与 Advance 同一个时间基准的毫秒），给主循环算休眠时长用：
    //   timeout = max(0, *NextDeadlineMs() - now)。
    // 是下界：不会晚于真正的下一次触发，但上层轮里的定时器会先在更早的时刻被"降级"一次，所以可能提前醒来；醒来后
    // Advance 没有回调可触发也没关系，再算一次即可。没有定时器、或还没调用过 Advance 确定基准时返回 nullopt。
    std::optional<std::int64_t> NextDeadlineMs() const;

    // 已分配的节点数（含空闲的），诊断用：反复 Add / Cancel 不应让它增长。
    std::size_t allocated_nodes() const noexcept { return node_count_; }
    // 当前 tick（自第一次 Advance 的基准起算）。回调里是正在处理的那个 tick，诊断 / 测试用。
    std::int64_t current_tick() const noexcept { return cur_tick_; }

    static constexpr int kLevels = 4;
    static constexpr int kSlotBits = 8;
    static constexpr int kSlots = 1 << kSlotBits;

private:
    enum class State : std::uint8_t
    {
        kFree,
        kPending, // 挂在某个槽的链表里
        kDue,     // 已从槽里摘下，在 due_ 里等着本 tick 触发
        kFiring,  // 回调正在执行
    };

    struct Node
    {
        Callback cb;
        std::int64_t expire = 0;   // 绝对 tick
        std::int64_t interval = 0; // tick 数，0 = 一次性
        std::uint64_t seq = 0;     // 同一 tick 内的触发顺序
        std::uint32_t prev = 0;
        std::uint32_t next = 0;
        std::uint32_t gen = 1;
        std::uint16_t bucket = 0; // level * kSlots + slot，仅 kPending 有意义
        State state = State::kFree;
        Repeat repeat = Repeat::kSkipMissed;
        bool stop_repeat = false; // 重复定时器在自己的回调里被取消
    };

    struct DueEntry
    {
        std::uint32_t idx;
        std::uint32_t gen;
        std::uint64_t seq;
    };

    static constexpr std::uint32_t kNil = 0xffffffffu;
    static constexpr std::uint16_t kNoBucket = 0xffffu;
    static constexpr int kChunkBits = 10;
    static constexpr std::uint32_t kChunkSize = 1u << kChunkBits;
    static constexpr std::int64_t kMaxTicks = std::int64_t{1} << 60;

    Node& At(std::uint32_t idx) { return chunks_[idx >> kChunkBits][idx & (kChunkSize - 1)]; }
    const Node& At(std::uint32_t idx) const { return chunks_[idx >> kChunkBits][idx & (kChunkSize - 1)]; }

    std::int64_t ToTicks(std::int64_t ms) const noexcept;
    std::uint32_t AllocNode();
    void FreeNode(std::uint32_t idx);
    TimerId MakeId(std::uint32_t idx) const { return (std::uint64_t{At(idx).gen} << 32) | (std::uint64_t{idx} + 1); }
    // 句柄 -> 节点下标；失效返回 kNil。
    std::uint32_t Resolve(TimerId id) const;
    TimerId AddNode(std::int64_t delay_ms, bool repeating, Callback cb, Repeat policy);

    void Schedule(std::uint32_t idx);
    void LinkTo(std::uint32_t idx, int level, int slot);
    void Unlink(std::uint32_t idx);
    void Cascade(int level, int slot);

    // 从 start 槽起环形查找第一个非空槽，返回距离（0..255），没有返回 -1。
    int FirstOccupiedFrom(int level, int start) const;
    // 下一个需要处理的 tick（有到期的定时器，或需要把上层槽降级）；没有返回 kNoTick。
    static constexpr std::int64_t kNoTick = INT64_MAX;
    std::int64_t NextTick() const;

    void ProcessTick(std::int64_t tick, std::size_t& fired);
    void DrainDue(std::size_t& fired);
    void Fire(std::uint32_t idx);
    void Finish(std::uint32_t idx);

    std::int64_t tick_ms_;
    std::int64_t base_ms_ = 0;
    std::int64_t cur_tick_ = 0;
    bool initialized_ = false;
    bool advancing_ = false;
    std::int64_t advance_target_ = 0;
    std::uint64_t next_seq_ = 1;
    std::size_t size_ = 0;

    std::vector<std::unique_ptr<Node[]>> chunks_; // 节点地址稳定：回调执行期间 AddTimer 触发扩容也不会搬动它
    std::uint32_t node_count_ = 0;
    std::uint32_t free_head_ = kNil;

    std::uint32_t head_[kLevels][kSlots];
    std::uint64_t occupied_[kLevels][kSlots / 64] = {};

    std::vector<DueEntry> due_;
    std::size_t due_pos_ = 0;
    std::uint32_t firing_ = kNil; // 回调正在执行的节点
};

} // namespace breeze
