// Swiss table 开放寻址哈希表的核心，FlatHashMap / FlatHashSet 共用（Policy 决定槽里存什么）。
// 思路与 Abseil 的 flat_hash_map 一致，但精简：不支持自定义分配器，没有按表的随机种子。
//
// 布局：一块内存 = 控制字节（capacity + kWidth 个）+ 槽数组。每个槽一个控制字节：满（存哈希低 7 位）/ 空 / 墓碑。
// 控制字节数组末尾复制了开头的 kWidth - 1 个，这样从任意位置读一整组（16 或 8 个）都不会越界，也不用处理回绕。
// 查找：按哈希高位定位到一组，用一条 SIMD 指令同时比较一组的 H2，只对命中的槽调 key_equal；
// 组内有"空"就说明这条探测序列上不会再有它了。探测按三角数步进（每次多走一组），访问到所有组。
//
// 容量是 2 的幂，最小 kWidth；最大装载 7/8。删除时，若那个槽前后各有一组大小的窗口里都有空位、且连续的满槽不足一组，
// 就没有任何探测序列越过过它，可以直接置空，否则留墓碑（这样删/插交替的负载不会攒墓碑）。
// 墓碑多到用完装载余量时：表不满就原容量重建（清掉墓碑），否则翻倍。

#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <new>
#include <type_traits>
#include <utility>

#include "hash_common.h"
#include "swiss_group.h"

namespace breeze::container_internal
{

inline constexpr std::size_t H1(std::size_t hash) noexcept { return hash >> 7; }
inline constexpr ctrl_t H2(std::size_t hash) noexcept { return static_cast<ctrl_t>(hash & 0x7f); }

// 容量 -> 最多能放几个元素（7/8）。
inline constexpr std::size_t CapacityToGrowth(std::size_t capacity) noexcept { return capacity - capacity / 8; }

// 放得下 n 个元素的最小容量（2 的幂，不小于一组）。
inline std::size_t CapacityForCount(std::size_t n) noexcept
{
    std::size_t cap = kGroupWidth;
    while (CapacityToGrowth(cap) < n)
        cap *= 2;
    return cap;
}

class ProbeSeq
{
public:
    ProbeSeq(std::size_t h1, std::size_t mask) noexcept
        : mask_(mask)
        , offset_(h1 & mask)
    {
    }
    std::size_t offset() const noexcept { return offset_; }
    std::size_t offset(std::size_t i) const noexcept { return (offset_ + i) & mask_; }
    void next() noexcept
    {
        index_ += kGroupWidth;
        offset_ = (offset_ + index_) & mask_;
    }

private:
    std::size_t mask_;
    std::size_t offset_;
    std::size_t index_ = 0;
};

// Policy 需要提供：
//   key_type / value_type（对外暴露的元素类型）/ slot_type（槽里实际存的）
//   static const key_type& key(const slot_type&)
//   static value_type& element(slot_type&) / const value_type& element(const slot_type&)
//   template <class... A> static void construct(slot_type*, A&&...)
//   static void construct_copy(slot_type* dst, const slot_type& src)
//   static void destroy(slot_type*)
//   static void transfer(slot_type* dst, slot_type* src)   // 在 dst 上移动构造并销毁 src
template <typename Policy, typename Hash, typename Eq>
class RawHashTable
{
public:
    using key_type = typename Policy::key_type;
    using value_type = typename Policy::value_type;
    using hasher = Hash;
    using key_equal = Eq;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using reference = value_type&;
    using const_reference = const value_type&;
    using pointer = value_type*;
    using const_pointer = const value_type*;

protected:
    using slot_type = typename Policy::slot_type;
    // 搬槽（扩容 / 重建）时不能抛：搬到一半抛出会留下一半搬了一半没搬的表。
    static_assert(Policy::kNothrowTransfer,
                  "键与值的移动构造函数必须是 noexcept（扩容时要搬动元素）；给自己的类型加上 noexcept");

    static constexpr bool kTransparent = HasIsTransparent<Hash>::value && HasIsTransparent<Eq>::value;
    // 透明的哈希与比较器：任何类型 K 都能用来查找；否则键参数的类型固定为 key_type（别的类型靠隐式转换）。
    template <typename K>
    using key_arg = typename KeyArg<kTransparent>::template type<K, key_type>;

public:
    // ---- 迭代器 ----

    template <bool Const>
    class Iter
    {
        using Table = std::conditional_t<Const, const RawHashTable, RawHashTable>;
        // 引用类型取决于策略给出的元素引用：map 是 pair<const K, V>&，set 永远是 const K&。
        using MutableRef = decltype(Policy::element(std::declval<typename Policy::slot_type&>()));
        using ConstRef = decltype(Policy::element(std::declval<const typename Policy::slot_type&>()));

    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = typename RawHashTable::value_type;
        using difference_type = std::ptrdiff_t;
        using reference = std::conditional_t<Const, ConstRef, MutableRef>;
        using pointer = std::add_pointer_t<reference>;

        Iter() noexcept = default;
        // iterator -> const_iterator
        template <bool C = Const, typename = std::enable_if_t<C>>
        Iter(const Iter<false>& o) noexcept
            : table_(o.table_)
            , idx_(o.idx_)
        {
        }

        reference operator*() const noexcept { return Policy::element(table_->slots_[idx_]); }
        pointer operator->() const noexcept { return &Policy::element(table_->slots_[idx_]); }
        Iter& operator++() noexcept
        {
            ++idx_;
            // 紧挨着的下一个槽是满的（装载高时最常见）就不用读一整组
            if (idx_ < table_->capacity_ && IsFull(table_->ctrl_[idx_]))
                return *this;
            SkipToFull();
            return *this;
        }
        Iter operator++(int) noexcept
        {
            Iter tmp = *this;
            ++*this;
            return tmp;
        }
        friend bool operator==(const Iter& a, const Iter& b) noexcept
        {
            return a.idx_ == b.idx_ && a.table_ == b.table_;
        }

    private:
        friend class RawHashTable;
        template <bool>
        friend class Iter;
        Iter(Table* t, std::size_t idx) noexcept
            : table_(t)
            , idx_(idx)
        {
        }
        Iter(Table* t, std::size_t idx, int /*skip*/) noexcept
            : table_(t)
            , idx_(idx)
        {
            SkipToFull();
        }

        // 一组一组地跳过非满的槽。读一整组时尾部可能落进控制字节末尾的那段拷贝（对应槽 0..kWidth-2），
        // 命中它们时 idx >= capacity，等同于没找到。
        void SkipToFull() noexcept
        {
            while (idx_ < table_->capacity_)
            {
                const Group g(table_->ctrl_ + idx_);
                if (auto full = g.MatchFull())
                {
                    idx_ += full.LowestBitSet();
                    if (idx_ >= table_->capacity_)
                        idx_ = table_->capacity_;
                    return;
                }
                idx_ += kGroupWidth;
            }
            idx_ = table_->capacity_;
        }

        Table* table_ = nullptr;
        std::size_t idx_ = 0;
    };

    using iterator = Iter<false>;
    using const_iterator = Iter<true>;

    // ---- 构造 / 析构 ----

    RawHashTable() = default;
    explicit RawHashTable(const Hash& h, const Eq& e = Eq())
        : hash_(h)
        , eq_(e)
    {
    }

    RawHashTable(const RawHashTable& o)
        : hash_(o.hash_)
        , eq_(o.eq_)
    {
        CopyFrom(o);
    }

    RawHashTable(RawHashTable&& o) noexcept
        : ctrl_(std::exchange(o.ctrl_, nullptr))
        , slots_(std::exchange(o.slots_, nullptr))
        , capacity_(std::exchange(o.capacity_, 0))
        , size_(std::exchange(o.size_, 0))
        , growth_left_(std::exchange(o.growth_left_, 0))
        , hash_(std::move(o.hash_))
        , eq_(std::move(o.eq_))
    {
    }

    RawHashTable& operator=(const RawHashTable& o)
    {
        if (this != &o)
        {
            RawHashTable tmp(o);
            swap(tmp);
        }
        return *this;
    }

    RawHashTable& operator=(RawHashTable&& o) noexcept
    {
        if (this != &o)
        {
            Release();
            ctrl_ = std::exchange(o.ctrl_, nullptr);
            slots_ = std::exchange(o.slots_, nullptr);
            capacity_ = std::exchange(o.capacity_, 0);
            size_ = std::exchange(o.size_, 0);
            growth_left_ = std::exchange(o.growth_left_, 0);
            hash_ = std::move(o.hash_);
            eq_ = std::move(o.eq_);
        }
        return *this;
    }

    ~RawHashTable() { Release(); }

    void swap(RawHashTable& o) noexcept
    {
        using std::swap;
        swap(ctrl_, o.ctrl_);
        swap(slots_, o.slots_);
        swap(capacity_, o.capacity_);
        swap(size_, o.size_);
        swap(growth_left_, o.growth_left_);
        swap(hash_, o.hash_);
        swap(eq_, o.eq_);
    }

    // ---- 迭代 ----

    iterator begin() noexcept { return iterator(this, 0, 0); }
    iterator end() noexcept { return iterator(this, capacity_); }
    const_iterator begin() const noexcept { return const_iterator(this, 0, 0); }
    const_iterator end() const noexcept { return const_iterator(this, capacity_); }
    const_iterator cbegin() const noexcept { return begin(); }
    const_iterator cend() const noexcept { return end(); }

    // ---- 容量 ----

    bool empty() const noexcept { return size_ == 0; }
    size_type size() const noexcept { return size_; }
    size_type capacity() const noexcept { return capacity_; }
    size_type bucket_count() const noexcept { return capacity_; }
    // 不扩容最多还能放多少个。
    size_type max_load() const noexcept { return CapacityToGrowth(capacity_); }
    double load_factor() const noexcept
    {
        return capacity_ == 0 ? 0.0 : static_cast<double>(size_) / static_cast<double>(capacity_);
    }
    static constexpr double max_load_factor() noexcept { return 0.875; }

    // 保证再放 n 个元素（总数达到 n）之前不扩容。
    void reserve(size_type n)
    {
        if (n > size_ + growth_left_)
            Resize(CapacityForCount(n));
    }
    // 把容量调整到至少放得下 n 个元素与现有元素；n = 0 且表空时释放存储。
    void rehash(size_type n)
    {
        if (n == 0 && size_ == 0)
        {
            Release();
            return;
        }
        const std::size_t want = std::max(CapacityForCount(size_), CapacityForCount(n));
        if (want != capacity_)
            Resize(want);
    }
    // 收缩到刚好放得下现有元素。
    void shrink_to_fit() { rehash(0); }

    // ---- 修改 ----

    // 删除所有元素，保留容量。
    void clear() noexcept
    {
        if (capacity_ == 0)
            return;
        DestroyElements();
        std::memset(ctrl_, static_cast<unsigned char>(kEmpty), capacity_ + kGroupWidth);
        size_ = 0;
        growth_left_ = CapacityToGrowth(capacity_);
    }

    // 删除 pos 指向的元素，返回下一个。其它元素的迭代器与引用都不受影响（元素不会搬动）。
    iterator erase(const_iterator pos) noexcept
    {
        const std::size_t idx = pos.idx_;
        EraseAt(idx);
        iterator next(this, idx + 1, 0);
        return next;
    }
    iterator erase(iterator pos) noexcept { return erase(const_iterator(pos)); }
    iterator erase(const_iterator first, const_iterator last) noexcept
    {
        // 删除不搬动元素，所以 last 在循环中始终有效
        while (first != last)
            first = erase(first);
        return iterator(this, last.idx_);
    }

    template <typename K = key_type>
    size_type erase(const key_arg<K>& key)
    {
        const std::size_t idx = FindIndex(key);
        if (idx == kNpos)
            return 0;
        EraseAt(idx);
        return 1;
    }

    // ---- 查找 ----

    template <typename K = key_type>
    iterator find(const key_arg<K>& key) noexcept
    {
        const std::size_t idx = FindIndex(key);
        return idx == kNpos ? end() : iterator(this, idx);
    }
    template <typename K = key_type>
    const_iterator find(const key_arg<K>& key) const noexcept
    {
        const std::size_t idx = FindIndex(key);
        return idx == kNpos ? end() : const_iterator(this, idx);
    }
    template <typename K = key_type>
    bool contains(const key_arg<K>& key) const noexcept
    {
        return FindIndex(key) != kNpos;
    }
    template <typename K = key_type>
    size_type count(const key_arg<K>& key) const noexcept
    {
        return contains<K>(key) ? 1 : 0;
    }

    hasher hash_function() const { return hash_; }
    key_equal key_eq() const { return eq_; }

    // 墓碑（已删除但还占着探测位置的槽）个数，O(容量)。诊断 / 测试用：删除优化是否生效、重建后是否清干净。
    size_type tombstones() const noexcept
    {
        size_type n = 0;
        for (std::size_t i = 0; i < capacity_; ++i)
            n += IsDeleted(ctrl_[i]) ? 1 : 0;
        return n;
    }

    // 自检：控制字节镜像、满槽数 = size、增长余量、每个满槽都能被查到。O(容量)，给测试与排障用。
    bool InvariantsHold() const
    {
        if (capacity_ == 0)
            return ctrl_ == nullptr && slots_ == nullptr && size_ == 0 && growth_left_ == 0;
        if (capacity_ < kGroupWidth || !std::has_single_bit(capacity_))
            return false;
        for (std::size_t i = 0; i + 1 < kGroupWidth; ++i)
        {
            if (ctrl_[capacity_ + i] != ctrl_[i])
                return false;
        }
        std::size_t full = 0, empty = 0;
        for (std::size_t i = 0; i < capacity_; ++i)
        {
            if (IsFull(ctrl_[i]))
                ++full;
            else if (IsEmpty(ctrl_[i]))
                ++empty;
        }
        if (full != size_ || growth_left_ + size_ > CapacityToGrowth(capacity_))
            return false;
        // 空槽数与装载余量同增同减（插进空槽两者都 -1，删成空槽两者都 +1，墓碑进出都不变），所以差是常数；
        // 由此空槽至少有 capacity/8 个，任何探测序列都会终止。
        if (empty != capacity_ - CapacityToGrowth(capacity_) + growth_left_)
            return false;
        for (std::size_t i = 0; i < capacity_; ++i)
        {
            if (!IsFull(ctrl_[i]))
                continue;
            const std::size_t h = HashOf(Policy::key(slots_[i]));
            if (ctrl_[i] != H2(h) || FindIndex(Policy::key(slots_[i])) != i)
                return false;
        }
        return true;
    }

protected:
    static constexpr std::size_t kNpos = static_cast<std::size_t>(-1);

    template <typename K>
    std::size_t HashOf(const K& key) const
    {
        return MixHash(static_cast<std::size_t>(hash_(key)));
    }

    template <typename K>
    std::size_t FindIndex(const K& key) const
    {
        if (capacity_ == 0)
            return kNpos;
        const std::size_t hash = HashOf(key);
        const ctrl_t h2 = H2(hash);
        ProbeSeq seq(H1(hash), capacity_ - 1);
        for (;;)
        {
            const Group g(ctrl_ + seq.offset());
            for (const std::uint32_t i : g.Match(h2))
            {
                const std::size_t idx = seq.offset(i);
                if (eq_(Policy::key(slots_[idx]), key)) [[likely]]
                    return idx;
            }
            if (g.MatchEmpty())
                return kNpos;
            seq.next();
        }
    }

    // 查找 key；找到返回它的下标（found =
    // true）；没找到返回一个可以放它的槽（必要时先扩容），此时表里还没有写入任何东西。 hash 带出 key
    // 的哈希，省得插入时再算一遍（字符串键的哈希不便宜）。
    template <typename K>
    std::size_t FindOrPrepareInsert(const K& key, std::size_t& hash_out, bool& found)
    {
        EnsureAllocated();
        const std::size_t hash = HashOf(key);
        hash_out = hash;
        const ctrl_t h2 = H2(hash);
        ProbeSeq seq(H1(hash), capacity_ - 1);
        std::size_t target = 0;
        bool have_target = false;
        for (;;)
        {
            const Group g(ctrl_ + seq.offset());
            for (const std::uint32_t i : g.Match(h2))
            {
                const std::size_t idx = seq.offset(i);
                if (eq_(Policy::key(slots_[idx]), key)) [[likely]]
                {
                    found = true;
                    return idx;
                }
            }
            if (!have_target)
            {
                if (auto m = g.MatchEmptyOrDeleted())
                {
                    target = seq.offset(m.LowestBitSet());
                    have_target = true;
                }
            }
            if (g.MatchEmpty())
                break; // 探测序列到头了，key 不在表里
            seq.next();
        }
        found = false;
        // 放进墓碑不消耗装载余量；放进空槽才消耗，没余量了先扩容（或清墓碑）再重新找位置
        if (growth_left_ == 0 && !IsDeleted(ctrl_[target]))
        {
            GrowOrRehash();
            target = FindFirstNonFull(hash);
        }
        return target;
    }

    // 在 idx 上原地构造元素并登记。必须紧跟在 FindOrPrepareInsert 返回 found = false 之后。
    // 先构造再改控制字节：构造抛异常时表里什么也没变。
    template <typename... Args>
    void ConstructAt(std::size_t idx, std::size_t hash, Args&&... args)
    {
        Policy::construct(&slots_[idx], std::forward<Args>(args)...);
        CommitInsert(idx, hash);
    }

    void CommitInsert(std::size_t idx, std::size_t hash) noexcept
    {
        if (IsEmpty(ctrl_[idx]))
            --growth_left_;
        SetCtrl(idx, H2(hash));
        ++size_;
    }

    // 先按 key 查，没有才用 construct_args 构造。返回 (迭代器, 是否新插入)。
    template <typename K, typename... CArgs>
    std::pair<iterator, bool> FindOrConstruct(const K& key, CArgs&&... construct_args)
    {
        bool found = false;
        std::size_t hash = 0;
        const std::size_t idx = FindOrPrepareInsert(key, hash, found);
        if (found)
            return {iterator(this, idx), false};
        ConstructAt(idx, hash, std::forward<CArgs>(construct_args)...);
        return {iterator(this, idx), true};
    }

    // 通用 emplace：先构造一个临时元素取出键，再决定插入还是丢弃。
    template <typename... Args>
    std::pair<iterator, bool> EmplaceGeneric(Args&&... args)
    {
        struct Temp
        {
            alignas(slot_type) unsigned char buf[sizeof(slot_type)];
            slot_type* slot() { return reinterpret_cast<slot_type*>(buf); }
            bool live = false;
            ~Temp()
            {
                if (live)
                    Policy::destroy(slot());
            }
        } temp;
        Policy::construct(temp.slot(), std::forward<Args>(args)...);
        temp.live = true;
        bool found = false;
        std::size_t hash = 0;
        const key_type& key = Policy::key(*temp.slot());
        const std::size_t idx = FindOrPrepareInsert(key, hash, found);
        if (found)
            return {iterator(this, idx), false};
        Policy::transfer(&slots_[idx], temp.slot()); // 搬进去并销毁临时的
        temp.live = false;
        CommitInsert(idx, hash);
        return {iterator(this, idx), true};
    }

    // 删除后的清理：前后都有空位且连续满槽不足一组 -> 没有探测序列越过它，置空并归还装载余量；否则留墓碑。
    void EraseAt(std::size_t idx) noexcept
    {
        Policy::destroy(&slots_[idx]);
        --size_;
        const std::size_t mask = capacity_ - 1;
        const std::size_t idx_before = (idx - kGroupWidth) & mask;
        const auto empty_after = Group(ctrl_ + idx).MatchEmpty();
        const auto empty_before = Group(ctrl_ + idx_before).MatchEmpty();
        const bool was_never_full =
            empty_before && empty_after && (empty_after.TrailingZeros() + empty_before.LeadingZeros() < kGroupWidth);
        SetCtrl(idx, was_never_full ? kEmpty : kDeleted);
        growth_left_ += was_never_full ? 1 : 0;
    }

    // ---- 存储 ----

    static constexpr std::size_t kAlign = alignof(slot_type) > 16 ? alignof(slot_type) : 16;

    static constexpr std::size_t SlotsOffset(std::size_t capacity) noexcept
    {
        const std::size_t ctrl_bytes = capacity + kGroupWidth;
        return (ctrl_bytes + alignof(slot_type) - 1) & ~(alignof(slot_type) - 1);
    }
    static constexpr std::size_t AllocSize(std::size_t capacity) noexcept
    {
        return SlotsOffset(capacity) + capacity * sizeof(slot_type);
    }

    void SetCtrl(std::size_t i, ctrl_t c) noexcept
    {
        ctrl_[i] = c;
        // 镜像：前 kWidth-1 个控制字节在末尾有拷贝。i >= kWidth-1 时下标算回 i 自己，重复写一次，省掉分支。
        ctrl_[((i - (kGroupWidth - 1)) & (capacity_ - 1)) + (kGroupWidth - 1)] = c;
    }

    // 分配一块新存储（全空），不碰 this 的成员。
    struct Storage
    {
        ctrl_t* ctrl;
        slot_type* slots;
    };
    static Storage AllocateStorage(std::size_t capacity)
    {
        char* mem = static_cast<char*>(::operator new(AllocSize(capacity), std::align_val_t{kAlign}));
        std::memset(mem, static_cast<unsigned char>(kEmpty), capacity + kGroupWidth);
        return {reinterpret_cast<ctrl_t*>(mem), reinterpret_cast<slot_type*>(mem + SlotsOffset(capacity))};
    }
    static void FreeStorage(ctrl_t* ctrl, std::size_t capacity) noexcept
    {
        ::operator delete(ctrl, AllocSize(capacity), std::align_val_t{kAlign});
    }

    void EnsureAllocated()
    {
        if (capacity_ == 0)
            Resize(kGroupWidth);
    }

    // 没有装载余量了：墓碑占了不少、实际元素不到 25/32 就原容量重建清墓碑，否则翻倍。
    void GrowOrRehash()
    {
        if (capacity_ > kGroupWidth && size_ * 32 <= capacity_ * 25)
            Resize(capacity_);
        else
            Resize(capacity_ * 2);
    }

    std::size_t FindFirstNonFull(std::size_t hash) const noexcept
    {
        ProbeSeq seq(H1(hash), capacity_ - 1);
        for (;;)
        {
            const Group g(ctrl_ + seq.offset());
            if (auto m = g.MatchEmptyOrDeleted())
                return seq.offset(m.LowestBitSet());
            seq.next();
        }
    }

    // 换成 new_capacity 的新存储，把现有元素搬过去（重新计算位置，墓碑随之消失）。
    void Resize(std::size_t new_capacity)
    {
        Storage fresh = AllocateStorage(new_capacity);
        ctrl_t* old_ctrl = ctrl_;
        slot_type* old_slots = slots_;
        const std::size_t old_capacity = capacity_;

        ctrl_ = fresh.ctrl;
        slots_ = fresh.slots;
        capacity_ = new_capacity;
        growth_left_ = CapacityToGrowth(new_capacity) - size_;

        for (std::size_t i = 0; i < old_capacity; ++i)
        {
            if (!IsFull(old_ctrl[i]))
                continue;
            const std::size_t hash = HashOf(Policy::key(old_slots[i]));
            const std::size_t target = FindFirstNonFull(hash);
            SetCtrl(target, H2(hash));
            Policy::transfer(&slots_[target], &old_slots[i]);
        }
        if (old_ctrl != nullptr)
            FreeStorage(old_ctrl, old_capacity);
    }

    void CopyFrom(const RawHashTable& o)
    {
        if (o.size_ == 0)
            return;
        const std::size_t cap = CapacityForCount(o.size_);
        Storage fresh = AllocateStorage(cap);
        ctrl_ = fresh.ctrl;
        slots_ = fresh.slots;
        capacity_ = cap;
        growth_left_ = CapacityToGrowth(cap);
        // 元素拷贝构造可能抛：析构函数不会跑（构造函数没完成），用守卫把已拷贝的销毁、存储释放
        struct Guard
        {
            RawHashTable* t;
            bool ok = false;
            ~Guard()
            {
                if (!ok)
                    t->Release();
            }
        } guard{this};
        for (std::size_t i = 0; i < o.capacity_; ++i)
        {
            if (!IsFull(o.ctrl_[i]))
                continue;
            const std::size_t hash = HashOf(Policy::key(o.slots_[i]));
            const std::size_t target = FindFirstNonFull(hash);
            Policy::construct_copy(&slots_[target], o.slots_[i]);
            CommitInsert(target, hash);
        }
        guard.ok = true;
    }

    void DestroyElements() noexcept
    {
        if constexpr (!std::is_trivially_destructible_v<slot_type>)
        {
            for (std::size_t i = 0; i < capacity_; ++i)
            {
                if (IsFull(ctrl_[i]))
                    Policy::destroy(&slots_[i]);
            }
        }
    }

    // 销毁全部元素并释放存储，回到未分配状态。
    void Release() noexcept
    {
        if (capacity_ == 0)
            return;
        DestroyElements();
        FreeStorage(ctrl_, capacity_);
        ctrl_ = nullptr;
        slots_ = nullptr;
        capacity_ = 0;
        size_ = 0;
        growth_left_ = 0;
    }

    ctrl_t* ctrl_ = nullptr;
    slot_type* slots_ = nullptr;
    std::size_t capacity_ = 0;
    std::size_t size_ = 0;
    std::size_t growth_left_ = 0;
    [[no_unique_address]] Hash hash_{};
    [[no_unique_address]] Eq eq_{};
};

} // namespace breeze::container_internal
