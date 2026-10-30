// 类型安全的同步事件总线：以 C++ 事件类型为键，发布时同步调用该类型的所有订阅者，不需要枚举或字符串。
//
//   struct PlayerLogin { std::int64_t player_id; };
//
//   EventBus bus;
//   SubId id = bus.Subscribe<PlayerLogin>([](const PlayerLogin& e) { ... });
//   auto guard = bus.SubscribeScoped<PlayerLogin>([this](const PlayerLogin& e) { OnLogin(e); }); // 析构时自动退订
//   bus.Publish(PlayerLogin{42});
//   bus.Unsubscribe(id);
//
// 语义：
//   - 订阅者按订阅顺序被调用。事件按**静态类型**分发：Publish(const Base&) 只通知订阅 Base 的，不通知订阅 Derived 的。
//   - 回调里可以随意 Subscribe / Unsubscribe / Clear / 再 Publish（别的类型或同一类型）：
//       本次 Publish 只通知"发布开始时已经存在"的订阅者，回调里新订阅的从下一次发布起生效；
//       回调里被退订、还没轮到的订阅者不会再被调用；回调里 Clear 之后新订阅的不受那次 Clear 影响。
//   - 嵌套发布的深度上限是 kMaxPublishDepth（跨类型也算：A 的回调发布 B、B 的回调又发布 A，防止无限递归）。
//     超限是编程错误：Debug 下 abort，Release 下丢弃这次发布并记日志（见 ASSERT_RET_VOID）。
//   - 回调抛异常：总线保持一致，异常原样传给 Publish 的调用方，这次发布里后面的订阅者不再被调用。
//   - 不是线程安全的：只在一个线程（通常是主逻辑线程）里用。跨线程要自己投递到主线程再 Publish。
//   - 事件对象只在 Publish 调用期间有效，回调里要保留就拷贝。
//   - ScopedSubscription 不能比它所属的 EventBus 活得更久（析构时要调用总线）。
//
// 实现要点（相对旧版）：订阅条目各自在堆上、地址稳定，回调执行期间订阅同一类型不会搬动正在运行的回调；
// 事件类型到订阅表是数组下标（每个类型一个进程内唯一的编号），不是哈希表；订阅句柄里直接带着类型编号，
// 退订不需要 "id -> 类型" 的映射表。

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "assert_macros.h"

namespace breeze
{

// 订阅句柄：高 24 位是事件类型编号 + 1，低 40 位是进程内唯一的序号（所以拿另一个总线的句柄来退订不会误伤）。0 = 无效。
using SubId = std::uint64_t;
inline constexpr SubId kInvalidSubId = 0;

class EventBus;

// 退订守卫：析构（或 Reset）时退订。只能移动。
class ScopedSubscription
{
public:
    ScopedSubscription() = default;
    ScopedSubscription(EventBus* bus, SubId id) noexcept
        : bus_(bus)
        , id_(id)
    {
    }
    ScopedSubscription(ScopedSubscription&& o) noexcept
        : bus_(std::exchange(o.bus_, nullptr))
        , id_(std::exchange(o.id_, kInvalidSubId))
    {
    }
    ScopedSubscription& operator=(ScopedSubscription&& o) noexcept
    {
        if (this != &o)
        {
            Reset();
            bus_ = std::exchange(o.bus_, nullptr);
            id_ = std::exchange(o.id_, kInvalidSubId);
        }
        return *this;
    }
    ScopedSubscription(const ScopedSubscription&) = delete;
    ScopedSubscription& operator=(const ScopedSubscription&) = delete;
    ~ScopedSubscription() { Reset(); }

    // 立刻退订（已退订则什么也不做）。
    void Reset() noexcept;
    // 放弃管理，不退订；返回句柄。
    SubId Release() noexcept
    {
        bus_ = nullptr;
        return std::exchange(id_, kInvalidSubId);
    }
    SubId id() const noexcept { return id_; }
    explicit operator bool() const noexcept { return bus_ != nullptr && id_ != kInvalidSubId; }

private:
    EventBus* bus_ = nullptr;
    SubId id_ = kInvalidSubId;
};

class EventBus
{
public:
    // 嵌套发布的最大深度。
    static constexpr int kMaxPublishDepth = 8;

    EventBus() = default;
    ~EventBus() = default;
    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;

    // 订阅事件类型 E。handler 是任何可以用 const E& 调用的东西（lambda、函数对象、std::function，可以是只能移动的）。
    template <typename E, typename F>
        requires std::invocable<F&, const E&>
    SubId Subscribe(F&& handler)
    {
        const std::size_t type = TypeIndex<E>();
        Slot& slot = SlotFor(type);
        const SubId id = MakeId(type);
        auto entry = std::make_unique<Entry>();
        entry->id = id;
        entry->fn = [h = std::forward<F>(handler)](const void* ev) mutable { h(*static_cast<const E*>(ev)); };
        slot.entries.push_back(std::move(entry));
        ++live_;
        return id;
    }

    template <typename E, typename F>
        requires std::invocable<F&, const E&>
    [[nodiscard]] ScopedSubscription SubscribeScoped(F&& handler)
    {
        return ScopedSubscription(this, Subscribe<E>(std::forward<F>(handler)));
    }

    // 同步通知 E 的所有订阅者。
    template <typename E>
    void Publish(const E& event)
    {
        const std::size_t type = TypeIndex<E>();
        if (type >= slots_.size() || !slots_[type])
            return;
        Slot& slot = *slots_[type];
        if (slot.entries.empty())
            return;
        ASSERT_RET_VOID(depth_ < kMaxPublishDepth); // 递归发布：Debug 崩，Release 丢弃并记日志

        PublishGuard guard(*this, slot);
        // 只通知发布开始时已经存在的订阅者。entries 里放的是 unique_ptr，
        // 回调里新订阅导致 vector 扩容也不会搬动 Entry 本身
        const std::size_t n = slot.entries.size();
        for (std::size_t i = 0; i < n; ++i)
        {
            Entry* e = slot.entries[i].get();
            if (!e->cancelled)
                e->fn(&event);
        }
    }

    // 退订。句柄无效、已经退订过、或来自别的总线，返回 false。
    bool Unsubscribe(SubId id)
    {
        Slot* slot = FindSlot(id);
        if (slot == nullptr)
            return false;
        for (auto& e : slot->entries)
        {
            if (e->id == id && !e->cancelled)
            {
                e->cancelled = true;
                --live_;
                slot->dirty = true;
                if (slot->publishing == 0)
                    Compact(*slot);
                return true;
            }
        }
        return false;
    }

    // 清空所有订阅。发布期间正在被调用的回调不会被销毁，而是标记取消、等那次发布结束后再清理。
    void Clear()
    {
        // 按下标遍历：Compact 会销毁回调，回调的析构函数里再订阅一个新类型会让 slots_ 扩容（Slot 本身在堆上，指针不变）
        for (std::size_t i = 0; i < slots_.size(); ++i)
        {
            Slot* slot = slots_[i].get();
            if (slot == nullptr)
                continue;
            for (auto& e : slot->entries)
            {
                if (!e->cancelled)
                {
                    e->cancelled = true;
                    --live_;
                }
            }
            slot->dirty = true;
            if (slot->publishing == 0)
                Compact(*slot);
        }
    }

    // 事件类型 E 现有的订阅者个数。
    template <typename E>
    std::size_t SubscriberCount() const
    {
        const std::size_t type = TypeIndex<E>();
        if (type >= slots_.size() || !slots_[type])
            return 0;
        std::size_t n = 0;
        for (const auto& e : slots_[type]->entries)
            n += e->cancelled ? 0 : 1;
        return n;
    }
    // 发布前先问一句，省得为没人订阅的事件构造昂贵的事件对象。
    template <typename E>
    bool HasSubscribers() const
    {
        return SubscriberCount<E>() != 0;
    }
    // 全部订阅者个数。
    std::size_t size() const noexcept { return live_; }
    bool empty() const noexcept { return live_ == 0; }

private:
    struct Entry
    {
        SubId id = kInvalidSubId;
        std::move_only_function<void(const void*)> fn;
        bool cancelled = false;
    };
    struct Slot
    {
        std::vector<std::unique_ptr<Entry>> entries;
        std::uint32_t publishing = 0; // 这个类型正在被发布的嵌套层数（>0 时不能动 entries 里的元素）
        bool dirty = false;           // 有已取消的条目等着清理
    };

    // 本次发布期间让深度与"正在发布"计数保持正确，回调抛异常也一样；最外层结束时清理已取消的条目。
    struct PublishGuard
    {
        PublishGuard(EventBus& b, Slot& s)
            : bus(b)
            , slot(s)
        {
            ++bus.depth_;
            ++slot.publishing;
        }
        ~PublishGuard()
        {
            --bus.depth_;
            if (--slot.publishing == 0 && slot.dirty)
                bus.Compact(slot);
        }
        EventBus& bus;
        Slot& slot;
    };

    static constexpr int kSeqBits = 40;
    static constexpr std::uint64_t kSeqMask = (std::uint64_t{1} << kSeqBits) - 1;

    // 每个事件类型一个进程内唯一的小整数。
    static std::size_t NextTypeIndex() noexcept
    {
        static std::atomic<std::size_t> next{0};
        return next.fetch_add(1, std::memory_order_relaxed);
    }
    template <typename E>
    static std::size_t TypeIndexOf() noexcept
    {
        static const std::size_t index = NextTypeIndex();
        return index;
    }
    template <typename E>
    static std::size_t TypeIndex() noexcept
    {
        return TypeIndexOf<std::remove_cvref_t<E>>(); // const E 与 E 是同一个事件类型
    }

    static SubId MakeId(std::size_t type) noexcept
    {
        static std::atomic<std::uint64_t> seq{0};
        const std::uint64_t s = seq.fetch_add(1, std::memory_order_relaxed) + 1;
        return ((static_cast<std::uint64_t>(type) + 1) << kSeqBits) | (s & kSeqMask);
    }

    Slot& SlotFor(std::size_t type)
    {
        if (type >= slots_.size())
            slots_.resize(type + 1);
        if (!slots_[type])
            slots_[type] =
                std::make_unique<Slot>(); // Slot 在堆上：回调里订阅新类型让 slots_ 扩容，不影响正在发布的那个 Slot
        return *slots_[type];
    }

    Slot* FindSlot(SubId id) noexcept
    {
        const std::uint64_t type_plus_one = id >> kSeqBits;
        if (type_plus_one == 0 || type_plus_one > slots_.size())
            return nullptr;
        return slots_[type_plus_one - 1].get();
    }

    // 删除已取消的条目。回调（及其捕获物）的析构放在状态整理好之后：
    // 它们的析构函数里再来 Subscribe / Unsubscribe 也是安全的。
    void Compact(Slot& slot)
    {
        std::vector<std::unique_ptr<Entry>> dead;
        std::vector<std::unique_ptr<Entry>> alive;
        alive.reserve(slot.entries.size());
        for (auto& e : slot.entries)
            (e->cancelled ? dead : alive).push_back(std::move(e));
        slot.entries = std::move(alive);
        slot.dirty = false;
        dead.clear();
    }

    std::vector<std::unique_ptr<Slot>> slots_;
    std::size_t live_ = 0;
    int depth_ = 0;
};

inline void ScopedSubscription::Reset() noexcept
{
    if (bus_ != nullptr && id_ != kInvalidSubId)
        bus_->Unsubscribe(id_);
    bus_ = nullptr;
    id_ = kInvalidSubId;
}

} // namespace breeze
