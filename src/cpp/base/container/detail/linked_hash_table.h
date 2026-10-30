// 保持插入顺序的哈希表核心，LinkedHashMap / LinkedHashSet 共用。
//
// 结构：元素存在一个紧凑的数组里（Entry：元素 + 哈希值 + 四个下标），哈希桶是一个下标数组，
// 桶内的冲突链与"插入顺序"链表都用下标串起来（32 位下标，所以最多约 42 亿个元素）。
//   - 查找：桶 -> 冲突链，比较存下的完整哈希值，相等再调 key_equal。
//   - 插入：追加到数组末尾，挂到冲突链头与顺序链尾；扩容只搬数组，下标不变，所以链不用重建（桶数组扩容才重建冲突链）。
//   - 删除：从两条链上摘下来，再把数组最后一个元素搬到空出来的位置（swap-and-pop），数组始终紧凑，删除是 O(1)。
//     冲突链与顺序链都是双向的，所以摘链和修补"被搬动的那个元素的邻居"都不需要遍历。
//   - 遍历：沿顺序链走。没有删除过时元素在数组里就是按顺序排的，遍历是顺序读。
//
// 迭代器 / 引用的失效：插入可能让全部失效（数组扩容）；删除一个元素会让它自己、以及被搬到它位置上的那个"原来最后插入
// 的元素"的迭代器与引用失效，其它不受影响（erase(iterator) 返回的"下一个"已经处理好这一点）。
//
// 槽策略见 slot_policy.h，与 FlatHash* 共用。

#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <new>
#include <type_traits>
#include <utility>

#include "hash_common.h"

namespace breeze::container_internal
{

template <typename Policy, typename Hash, typename Eq>
class LinkedHashTable
{
public:
    using key_type = typename Policy::key_type;
    using value_type = typename Policy::value_type;
    using hasher = Hash;
    using key_equal = Eq;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;

protected:
    using slot_type = typename Policy::slot_type;
    static_assert(Policy::kNothrowTransfer,
                  "键与值的移动构造函数必须是 noexcept（扩容、删除时要搬动元素）；给自己的类型加上 noexcept");

    using index_t = std::uint32_t;
    static constexpr index_t kNil = 0xffffffffu;
    static constexpr std::size_t kMaxEntries = 0xfffffffeu;

    static constexpr bool kTransparent = HasIsTransparent<Hash>::value && HasIsTransparent<Eq>::value;
    template <typename K>
    using key_arg = typename KeyArg<kTransparent>::template type<K, key_type>;

    struct Meta
    {
        std::size_t hash;
        index_t hash_next;
        index_t hash_prev;
        index_t link_prev;
        index_t link_next;
    };
    struct Entry
    {
        slot_type slot;
        Meta meta;
    };

public:
    // ---- 迭代器（双向，按插入顺序） ----

    template <bool Const>
    class Iter
    {
        using Table = std::conditional_t<Const, const LinkedHashTable, LinkedHashTable>;
        using MutableRef = decltype(Policy::element(std::declval<typename Policy::slot_type&>()));
        using ConstRef = decltype(Policy::element(std::declval<const typename Policy::slot_type&>()));

    public:
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type = typename LinkedHashTable::value_type;
        using difference_type = std::ptrdiff_t;
        using reference = std::conditional_t<Const, ConstRef, MutableRef>;
        using pointer = std::add_pointer_t<reference>;

        Iter() noexcept = default;
        template <bool C = Const, typename = std::enable_if_t<C>>
        Iter(const Iter<false>& o) noexcept
            : table_(o.table_)
            , idx_(o.idx_)
        {
        }

        reference operator*() const noexcept { return Policy::element(table_->entries_[idx_].slot); }
        pointer operator->() const noexcept { return &Policy::element(table_->entries_[idx_].slot); }
        Iter& operator++() noexcept
        {
            idx_ = table_->entries_[idx_].meta.link_next;
            return *this;
        }
        Iter operator++(int) noexcept
        {
            Iter tmp = *this;
            ++*this;
            return tmp;
        }
        Iter& operator--() noexcept
        {
            idx_ = idx_ == kNil ? table_->tail_ : table_->entries_[idx_].meta.link_prev; // end() 的前一个是最后插入的
            return *this;
        }
        Iter operator--(int) noexcept
        {
            Iter tmp = *this;
            --*this;
            return tmp;
        }
        friend bool operator==(const Iter& a, const Iter& b) noexcept
        {
            return a.idx_ == b.idx_ && a.table_ == b.table_;
        }

    private:
        friend class LinkedHashTable;
        template <bool>
        friend class Iter;
        Iter(Table* t, index_t idx) noexcept
            : table_(t)
            , idx_(idx)
        {
        }

        Table* table_ = nullptr;
        index_t idx_ = kNil;
    };

    using iterator = Iter<false>;
    using const_iterator = Iter<true>;
    using reverse_iterator = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    // ---- 构造 / 析构 ----

    LinkedHashTable() = default;
    explicit LinkedHashTable(const Hash& h, const Eq& e = Eq())
        : hash_(h)
        , eq_(e)
    {
    }

    LinkedHashTable(const LinkedHashTable& o)
        : hash_(o.hash_)
        , eq_(o.eq_)
    {
        CopyFrom(o);
    }

    LinkedHashTable(LinkedHashTable&& o) noexcept
        : hash_(std::move(o.hash_))
        , eq_(std::move(o.eq_))
    {
        StealFrom(o);
    }

    LinkedHashTable& operator=(const LinkedHashTable& o)
    {
        if (this != &o)
        {
            LinkedHashTable tmp(o);
            swap(tmp);
        }
        return *this;
    }

    LinkedHashTable& operator=(LinkedHashTable&& o) noexcept
    {
        if (this != &o)
        {
            Release();
            hash_ = std::move(o.hash_);
            eq_ = std::move(o.eq_);
            StealFrom(o);
        }
        return *this;
    }

    ~LinkedHashTable() { Release(); }

    void swap(LinkedHashTable& o) noexcept
    {
        using std::swap;
        swap(entries_, o.entries_);
        swap(buckets_, o.buckets_);
        swap(size_, o.size_);
        swap(entries_cap_, o.entries_cap_);
        swap(bucket_count_, o.bucket_count_);
        swap(head_, o.head_);
        swap(tail_, o.tail_);
        swap(hash_, o.hash_);
        swap(eq_, o.eq_);
    }

    // ---- 迭代 ----

    iterator begin() noexcept { return iterator(this, head_); }
    iterator end() noexcept { return iterator(this, kNil); }
    const_iterator begin() const noexcept { return const_iterator(this, head_); }
    const_iterator end() const noexcept { return const_iterator(this, kNil); }
    const_iterator cbegin() const noexcept { return begin(); }
    const_iterator cend() const noexcept { return end(); }
    reverse_iterator rbegin() noexcept { return reverse_iterator(end()); }
    reverse_iterator rend() noexcept { return reverse_iterator(begin()); }
    const_reverse_iterator rbegin() const noexcept { return const_reverse_iterator(end()); }
    const_reverse_iterator rend() const noexcept { return const_reverse_iterator(begin()); }
    const_reverse_iterator crbegin() const noexcept { return rbegin(); }
    const_reverse_iterator crend() const noexcept { return rend(); }

    // 最先 / 最后插入的元素（表非空）。
    decltype(auto) front() noexcept { return *begin(); }
    decltype(auto) front() const noexcept { return *begin(); }
    decltype(auto) back() noexcept { return *iterator(this, tail_); }
    decltype(auto) back() const noexcept { return *const_iterator(this, tail_); }

    // ---- 容量 ----

    bool empty() const noexcept { return size_ == 0; }
    size_type size() const noexcept { return size_; }
    // 不扩容最多能放多少个。
    size_type capacity() const noexcept { return entries_cap_; }

    void reserve(size_type n)
    {
        if (n > entries_cap_)
            ReallocEntries(n);
        EnsureBuckets(n);
    }
    // 收缩到刚好放得下现有元素（空表释放存储）。
    void shrink_to_fit()
    {
        if (size_ == 0)
        {
            Release();
            return;
        }
        if (entries_cap_ > size_)
            ReallocEntries(size_);
        const std::size_t want = BucketsFor(size_);
        if (want < bucket_count_)
            RebuildBuckets(want);
    }

    // ---- 修改 ----

    void clear() noexcept
    {
        DestroyEntries();
        if (bucket_count_ != 0)
            std::memset(buckets_, 0xff, bucket_count_ * sizeof(index_t)); // kNil 全是 1 位
        size_ = 0;
        head_ = tail_ = kNil;
    }

    // 删除 pos，返回按插入顺序的下一个。
    iterator erase(const_iterator pos) noexcept
    {
        const index_t idx = pos.idx_;
        index_t next = entries_[idx].meta.link_next;
        const index_t last = static_cast<index_t>(size_ - 1);
        if (next == last && idx != last)
            next = idx; // 下一个元素正是要被搬到 idx 的那个
        EraseAt(idx);
        return iterator(this, next);
    }
    iterator erase(iterator pos) noexcept { return erase(const_iterator(pos)); }
    iterator erase(const_iterator first, const_iterator last) noexcept
    {
        // 每删一个，数组最后一个元素会被搬到空位：如果它恰好是 last 指向的（或 first 的下一个），下标要跟着改
        while (first != last)
        {
            const index_t idx = first.idx_;
            const index_t moved = static_cast<index_t>(size_ - 1);
            index_t next = entries_[idx].meta.link_next;
            if (idx != moved)
            {
                if (next == moved)
                    next = idx;
                if (last.idx_ == moved)
                    last.idx_ = idx;
            }
            EraseAt(idx);
            first = const_iterator(this, next);
        }
        return iterator(this, last.idx_);
    }

    template <typename K = key_type>
    size_type erase(const key_arg<K>& key)
    {
        const index_t idx = FindIndex(key, HashOf(key));
        if (idx == kNil)
            return 0;
        EraseAt(idx);
        return 1;
    }

    // 删除最先 / 最后插入的元素（表非空）。LRU 淘汰最久没用的就是 pop_front。
    void pop_front() noexcept { EraseAt(head_); }
    void pop_back() noexcept { EraseAt(tail_); }

    // 把元素挪到顺序的末尾 / 开头，不改变其它元素的相对顺序，O(1)。迭代器 / 引用不受影响。
    // LRU：每次命中就 move_to_back，淘汰就 pop_front。
    void move_to_back(const_iterator pos) noexcept
    {
        const index_t idx = pos.idx_;
        if (idx == tail_)
            return;
        UnlinkOrder(idx);
        LinkOrderBack(idx);
    }
    void move_to_front(const_iterator pos) noexcept
    {
        const index_t idx = pos.idx_;
        if (idx == head_)
            return;
        UnlinkOrder(idx);
        LinkOrderFront(idx);
    }

    // ---- 查找 ----

    template <typename K = key_type>
    iterator find(const key_arg<K>& key) noexcept
    {
        return iterator(this, FindIndex(key, HashOf(key)));
    }
    template <typename K = key_type>
    const_iterator find(const key_arg<K>& key) const noexcept
    {
        return const_iterator(this, FindIndex(key, HashOf(key)));
    }
    template <typename K = key_type>
    bool contains(const key_arg<K>& key) const noexcept
    {
        return FindIndex(key, HashOf(key)) != kNil;
    }
    template <typename K = key_type>
    size_type count(const key_arg<K>& key) const noexcept
    {
        return contains<K>(key) ? 1 : 0;
    }

    hasher hash_function() const { return hash_; }
    key_equal key_eq() const { return eq_; }

    // 自检：两条链的完整性（顺序链 size 个、正反一致、冲突链各元素落在正确的桶里、总数 =
    // size）、数组紧凑、每个元素可被查到。
    bool InvariantsHold() const
    {
        if (entries_cap_ == 0)
            return entries_ == nullptr && size_ == 0 && head_ == kNil && tail_ == kNil;
        if (size_ > entries_cap_ || (size_ == 0) != (head_ == kNil) || (head_ == kNil) != (tail_ == kNil))
            return false;
        if (size_ != 0 && (bucket_count_ == 0 || !std::has_single_bit(bucket_count_) || size_ > bucket_count_))
            return false;
        // 顺序链：从头走到尾恰好 size 个，prev 与 next 互逆
        std::size_t n = 0;
        index_t prev = kNil;
        for (index_t i = head_; i != kNil; i = entries_[i].meta.link_next)
        {
            if (i >= size_ || n >= size_ || entries_[i].meta.link_prev != prev)
                return false;
            prev = i;
            ++n;
        }
        if (n != size_ || prev != tail_)
            return false;
        // 冲突链：每个桶里的元素哈希落在这个桶、prev/next 互逆；所有桶合起来恰好 size 个
        std::size_t in_chains = 0;
        for (std::size_t b = 0; b < bucket_count_; ++b)
        {
            index_t p = kNil;
            for (index_t i = buckets_[b]; i != kNil; i = entries_[i].meta.hash_next)
            {
                if (i >= size_ || BucketOf(entries_[i].meta.hash) != b || entries_[i].meta.hash_prev != p ||
                    in_chains >= size_)
                    return false;
                p = i;
                ++in_chains;
            }
        }
        if (in_chains != size_)
            return false;
        for (index_t i = 0; i < size_; ++i)
        {
            if (entries_[i].meta.hash != HashOf(Policy::key(entries_[i].slot)) ||
                FindIndex(Policy::key(entries_[i].slot), entries_[i].meta.hash) != i)
            {
                return false;
            }
        }
        return true;
    }

protected:
    template <typename K>
    std::size_t HashOf(const K& key) const
    {
        return MixHash(static_cast<std::size_t>(hash_(key)));
    }
    std::size_t BucketOf(std::size_t hash) const noexcept { return hash & (bucket_count_ - 1); }

    template <typename K>
    index_t FindIndex(const K& key, std::size_t hash) const
    {
        if (bucket_count_ == 0)
            return kNil;
        for (index_t i = buckets_[BucketOf(hash)]; i != kNil; i = entries_[i].meta.hash_next)
        {
            const Meta& m = entries_[i].meta;
            if (m.hash == hash && eq_(Policy::key(entries_[i].slot), key))
                return i;
        }
        return kNil;
    }

    // 先按 key 查，没有才用 construct_args 追加一个新元素到顺序末尾。返回 (迭代器, 是否新插入)。
    template <typename K, typename... CArgs>
    std::pair<iterator, bool> FindOrConstruct(const K& key, CArgs&&... construct_args)
    {
        const std::size_t hash = HashOf(key);
        const index_t found = FindIndex(key, hash);
        if (found != kNil)
            return {iterator(this, found), false};
        PrepareAppend();
        const index_t idx = static_cast<index_t>(size_);
        Policy::construct(&entries_[idx].slot, std::forward<CArgs>(construct_args)...); // 抛异常时表里什么也没变
        CommitAppend(idx, hash);
        return {iterator(this, idx), true};
    }

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
        const key_type& key = Policy::key(*temp.slot());
        const std::size_t hash = HashOf(key);
        const index_t found = FindIndex(key, hash);
        if (found != kNil)
            return {iterator(this, found), false};
        PrepareAppend();
        const index_t idx = static_cast<index_t>(size_);
        Policy::transfer(&entries_[idx].slot, temp.slot());
        temp.live = false;
        CommitAppend(idx, hash);
        return {iterator(this, idx), true};
    }

    // ---- 链操作 ----

    Meta& M(index_t i) noexcept { return entries_[i].meta; }
    const Meta& M(index_t i) const noexcept { return entries_[i].meta; }

    void LinkOrderBack(index_t idx) noexcept
    {
        Meta& m = M(idx);
        m.link_prev = tail_;
        m.link_next = kNil;
        if (tail_ != kNil)
            M(tail_).link_next = idx;
        else
            head_ = idx;
        tail_ = idx;
    }
    void LinkOrderFront(index_t idx) noexcept
    {
        Meta& m = M(idx);
        m.link_prev = kNil;
        m.link_next = head_;
        if (head_ != kNil)
            M(head_).link_prev = idx;
        else
            tail_ = idx;
        head_ = idx;
    }
    void UnlinkOrder(index_t idx) noexcept
    {
        const Meta& m = M(idx);
        if (m.link_prev != kNil)
            M(m.link_prev).link_next = m.link_next;
        else
            head_ = m.link_next;
        if (m.link_next != kNil)
            M(m.link_next).link_prev = m.link_prev;
        else
            tail_ = m.link_prev;
    }
    void LinkHashFront(index_t idx) noexcept
    {
        Meta& m = M(idx);
        index_t& head = buckets_[BucketOf(m.hash)];
        m.hash_prev = kNil;
        m.hash_next = head;
        if (head != kNil)
            M(head).hash_prev = idx;
        head = idx;
    }
    void UnlinkHash(index_t idx) noexcept
    {
        const Meta& m = M(idx);
        if (m.hash_prev != kNil)
            M(m.hash_prev).hash_next = m.hash_next;
        else
            buckets_[BucketOf(m.hash)] = m.hash_next;
        if (m.hash_next != kNil)
            M(m.hash_next).hash_prev = m.hash_prev;
    }

    // 新元素放在数组末尾 idx = size_；构造成功后登记到两条链里。
    void CommitAppend(index_t idx, std::size_t hash) noexcept
    {
        M(idx).hash = hash;
        LinkHashFront(idx);
        LinkOrderBack(idx);
        ++size_;
    }

    // 追加之前保证数组与桶都放得下 size_ + 1 个。
    void PrepareAppend()
    {
        if (size_ >= kMaxEntries)
        {
            std::fprintf(stderr, "LinkedHashTable: too many elements\n");
            std::abort();
        }
        if (size_ == entries_cap_)
            ReallocEntries(std::max<std::size_t>(8, entries_cap_ * 2));
        EnsureBuckets(size_ + 1);
    }

    void EraseAt(index_t idx) noexcept
    {
        UnlinkHash(idx);
        UnlinkOrder(idx);
        Policy::destroy(&entries_[idx].slot);
        const index_t last = static_cast<index_t>(size_ - 1);
        if (idx != last)
        {
            // 把最后一个元素搬到空位，并修补邻居对它的引用（下标变了）
            Policy::transfer(&entries_[idx].slot, &entries_[last].slot);
            entries_[idx].meta = entries_[last].meta;
            const Meta& m = M(idx);
            if (m.hash_prev != kNil)
                M(m.hash_prev).hash_next = idx;
            else
                buckets_[BucketOf(m.hash)] = idx;
            if (m.hash_next != kNil)
                M(m.hash_next).hash_prev = idx;
            if (m.link_prev != kNil)
                M(m.link_prev).link_next = idx;
            else
                head_ = idx;
            if (m.link_next != kNil)
                M(m.link_next).link_prev = idx;
            else
                tail_ = idx;
        }
        --size_;
    }

    // ---- 存储 ----

    static Entry* AllocEntries(std::size_t n)
    {
        return static_cast<Entry*>(::operator new(n * sizeof(Entry), std::align_val_t{alignof(Entry)}));
    }
    static void FreeEntries(Entry* p, std::size_t n) noexcept
    {
        ::operator delete(p, n * sizeof(Entry), std::align_val_t{alignof(Entry)});
    }

    // 数组换成容量 new_cap 的新存储，元素按下标搬过去（下标不变，所以链不用动）。
    void ReallocEntries(std::size_t new_cap)
    {
        Entry* fresh = AllocEntries(new_cap);
        for (std::size_t i = 0; i < size_; ++i)
        {
            Policy::transfer(&fresh[i].slot, &entries_[i].slot);
            fresh[i].meta = entries_[i].meta;
        }
        if (entries_ != nullptr)
            FreeEntries(entries_, entries_cap_);
        entries_ = fresh;
        entries_cap_ = new_cap;
    }

    // 桶数 = 不小于 n * 4 / 3 的 2 的幂（装载因子 <= 0.75），至少 8。
    static std::size_t BucketsFor(std::size_t n) noexcept
    {
        std::size_t b = 8;
        while (b * 3 < n * 4)
            b *= 2;
        return b;
    }
    void EnsureBuckets(std::size_t n)
    {
        if (bucket_count_ == 0 || n * 4 > bucket_count_ * 3)
            RebuildBuckets(std::max(BucketsFor(n), bucket_count_));
    }
    // 换一个桶数组，从元素数组重建所有冲突链。
    void RebuildBuckets(std::size_t new_count)
    {
        index_t* fresh = static_cast<index_t*>(::operator new(new_count * sizeof(index_t)));
        std::memset(fresh, 0xff, new_count * sizeof(index_t));
        if (buckets_ != nullptr)
            ::operator delete(buckets_);
        buckets_ = fresh;
        bucket_count_ = new_count;
        for (index_t i = 0; i < size_; ++i)
            LinkHashFront(i);
    }

    // 按插入顺序拷贝：新表里元素的数组位置就是插入顺序。
    void CopyFrom(const LinkedHashTable& o)
    {
        if (o.size_ == 0)
            return;
        entries_ = AllocEntries(o.size_);
        entries_cap_ = o.size_;
        RebuildBuckets(BucketsFor(o.size_)); // size_ 此时为 0，只分配并清空桶
        struct Guard
        {
            LinkedHashTable* t;
            bool ok = false;
            ~Guard()
            {
                if (!ok)
                    t->Release();
            }
        } guard{this};
        for (index_t i = o.head_; i != kNil; i = o.entries_[i].meta.link_next)
        {
            const index_t idx = static_cast<index_t>(size_);
            Policy::construct_copy(&entries_[idx].slot, o.entries_[i].slot);
            CommitAppend(idx, o.entries_[i].meta.hash);
        }
        guard.ok = true;
    }

    void StealFrom(LinkedHashTable& o) noexcept
    {
        entries_ = std::exchange(o.entries_, nullptr);
        buckets_ = std::exchange(o.buckets_, nullptr);
        size_ = std::exchange(o.size_, 0);
        entries_cap_ = std::exchange(o.entries_cap_, 0);
        bucket_count_ = std::exchange(o.bucket_count_, 0);
        head_ = std::exchange(o.head_, kNil); // 链头尾也要复位：被移走的表要回到完整的"空"状态
        tail_ = std::exchange(o.tail_, kNil);
    }

    void DestroyEntries() noexcept
    {
        if constexpr (!std::is_trivially_destructible_v<slot_type>)
        {
            for (std::size_t i = 0; i < size_; ++i)
                Policy::destroy(&entries_[i].slot);
        }
    }

    void Release() noexcept
    {
        DestroyEntries();
        if (entries_ != nullptr)
            FreeEntries(entries_, entries_cap_);
        if (buckets_ != nullptr)
            ::operator delete(buckets_);
        entries_ = nullptr;
        buckets_ = nullptr;
        size_ = entries_cap_ = bucket_count_ = 0;
        head_ = tail_ = kNil;
    }

    Entry* entries_ = nullptr;
    index_t* buckets_ = nullptr;
    std::size_t size_ = 0;
    std::size_t entries_cap_ = 0;
    std::size_t bucket_count_ = 0;
    index_t head_ = kNil;
    index_t tail_ = kNil;
    [[no_unique_address]] Hash hash_{};
    [[no_unique_address]] Eq eq_{};
};

} // namespace breeze::container_internal
