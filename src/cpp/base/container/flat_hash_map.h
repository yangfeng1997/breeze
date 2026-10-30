// FlatHashMap：开放寻址的 Swiss table 哈希映射。键值直接存在一块连续内存里，没有每元素一次的堆分配，
// 查找一般只碰一两个缓存行；同样的数据量下比 std::unordered_map 快好几倍、省内存（见 docs/基础容器库.md 的实测）。
//
//   FlatHashMap<uint64_t, Player> players;
//   players.try_emplace(id, name, level);       // 不存在才构造
//   if (auto it = players.find(id); it != players.end()) it->second.level++;
//   players.erase(id);
//
// 与 std::unordered_map 的区别（都是为了快，用之前要知道）：
//   - **插入可能让所有迭代器、指针、引用失效**（扩容要搬动元素）；不扩容的插入与任何删除都不影响其它元素。
//     reserve(n) 之后放 n 个以内不会扩容。要地址稳定就存 unique_ptr<T> / 用 std::unordered_map。
//   - 遍历顺序没有定义，而且插入、扩容后会变。
//   - 键与值的移动构造函数必须 noexcept（static_assert 会拦）。
//   - 没有 bucket / 节点接口（extract / merge / bucket_size …），没有自定义分配器。
//   - at() 找不到键时直接 abort（库不抛异常）；要"找不到就算了"用 find。
//   - 哈希函数是对象，可以带状态（如带种子的哈希）。**没有哈希洪泛防护**：键来自不可信输入（玩家名、请求参数）又被
//     故意构造碰撞时，用带随机种子的哈希。
//   - 键是 const 的，值可以改。
//
// 异构查找：Hash 与 KeyEqual 都声明了 is_transparent 时，find / contains / count / erase / at / operator[]
// 可以直接用别的 类型的键（典型：FlatHashMap<std::string, V, StringHash, std::equal_to<>> 用 string_view 查，不构造临时
// std::string）。

#pragma once

#include <functional>
#include <initializer_list>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>

#include "detail/raw_hash_table.h"
#include "detail/slot_policy.h"

namespace breeze
{

template <typename K, typename V, typename Hash = std::hash<K>, typename KeyEqual = std::equal_to<K>>
class FlatHashMap : public container_internal::RawHashTable<container_internal::MapPolicy<K, V>, Hash, KeyEqual>
{
    using Base = container_internal::RawHashTable<container_internal::MapPolicy<K, V>, Hash, KeyEqual>;

public:
    using key_type = K;
    using mapped_type = V;
    using typename Base::const_iterator;
    using typename Base::hasher;
    using typename Base::iterator;
    using typename Base::key_equal;
    using typename Base::size_type;
    using typename Base::value_type;

    FlatHashMap() = default;
    explicit FlatHashMap(size_type reserve_hint, const Hash& h = Hash(), const KeyEqual& e = KeyEqual())
        : Base(h, e)
    {
        this->reserve(reserve_hint);
    }
    FlatHashMap(std::initializer_list<value_type> init) { insert(init); }
    template <typename It>
        requires std::input_iterator<It>
    FlatHashMap(It first, It last)
    {
        insert(first, last);
    }
    FlatHashMap(const FlatHashMap&) = default;
    FlatHashMap(FlatHashMap&&) noexcept = default;
    FlatHashMap& operator=(const FlatHashMap&) = default;
    FlatHashMap& operator=(FlatHashMap&&) noexcept = default;
    FlatHashMap& operator=(std::initializer_list<value_type> init)
    {
        this->clear();
        insert(init);
        return *this;
    }

    // ---- 插入 ----

    // 键不存在才构造；存在时 args 原封不动（不会被移走）。返回 (指向该键的迭代器, 是否新插入)。
    template <typename... Args>
    std::pair<iterator, bool> try_emplace(const key_type& key, Args&&... args)
    {
        return this->FindOrConstruct(key,
                                     std::piecewise_construct,
                                     std::forward_as_tuple(key),
                                     std::forward_as_tuple(std::forward<Args>(args)...));
    }
    template <typename... Args>
    std::pair<iterator, bool> try_emplace(key_type&& key, Args&&... args)
    {
        return this->FindOrConstruct(key,
                                     std::piecewise_construct,
                                     std::forward_as_tuple(std::move(key)),
                                     std::forward_as_tuple(std::forward<Args>(args)...));
    }

    // 先构造一个临时元素再查重，所以键已存在时也付了一次构造的代价；能用 try_emplace 就用它。
    template <typename... Args>
    std::pair<iterator, bool> emplace(Args&&... args)
    {
        return this->EmplaceGeneric(std::forward<Args>(args)...);
    }

    std::pair<iterator, bool> insert(const value_type& v) { return try_emplace(v.first, v.second); }
    // 键是 const 的，只能拷贝，值可以移走。
    std::pair<iterator, bool> insert(value_type&& v) { return try_emplace(v.first, std::move(v.second)); }
    template <typename P>
        requires(std::is_constructible_v<value_type, P&&> && !std::is_same_v<std::remove_cvref_t<P>, value_type>)
    std::pair<iterator, bool> insert(P&& p)
    {
        return emplace(std::forward<P>(p));
    }
    template <typename It>
        requires std::input_iterator<It>
    void insert(It first, It last)
    {
        for (; first != last; ++first)
            insert(*first);
    }
    void insert(std::initializer_list<value_type> init) { insert(init.begin(), init.end()); }

    // 存在就赋值，不存在就插入。
    template <typename M>
    std::pair<iterator, bool> insert_or_assign(const key_type& key, M&& m)
    {
        auto r = try_emplace(key, std::forward<M>(m));
        if (!r.second)
            r.first->second = std::forward<M>(m);
        return r;
    }
    template <typename M>
    std::pair<iterator, bool> insert_or_assign(key_type&& key, M&& m)
    {
        auto r = try_emplace(std::move(key), std::forward<M>(m));
        if (!r.second)
            r.first->second = std::forward<M>(m);
        return r;
    }

    V& operator[](const key_type& key) { return try_emplace(key).first->second; }
    V& operator[](key_type&& key) { return try_emplace(std::move(key)).first->second; }

    // 找不到键直接 abort。
    template <typename Q = key_type>
    V& at(const typename Base::template key_arg<Q>& key)
    {
        const auto it = this->template find<Q>(key);
        if (it == this->end())
            container_internal::FatalKeyNotFound("FlatHashMap::at");
        return it->second;
    }
    template <typename Q = key_type>
    const V& at(const typename Base::template key_arg<Q>& key) const
    {
        const auto it = this->template find<Q>(key);
        if (it == this->end())
            container_internal::FatalKeyNotFound("FlatHashMap::at");
        return it->second;
    }

    friend void swap(FlatHashMap& a, FlatHashMap& b) noexcept { a.swap(b); }

    friend bool operator==(const FlatHashMap& a, const FlatHashMap& b)
    {
        if (a.size() != b.size())
            return false;
        for (const auto& [k, v] : a)
        {
            const auto it = b.find(k);
            if (it == b.end() || !(it->second == v))
                return false;
        }
        return true;
    }
};

// 删除所有满足条件的元素，返回删除个数。pred 的参数是 value_type&。
template <typename K, typename V, typename H, typename E, typename Pred>
typename FlatHashMap<K, V, H, E>::size_type erase_if(FlatHashMap<K, V, H, E>& m, Pred pred)
{
    const auto before = m.size();
    for (auto it = m.begin(); it != m.end();)
    {
        if (pred(*it))
            it = m.erase(it);
        else
            ++it;
    }
    return before - m.size();
}

} // namespace breeze
