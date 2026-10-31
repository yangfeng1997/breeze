// LinkedHashMap：哈希映射 + 按插入顺序遍历。查找是 O(1)，遍历按"谁先插入谁在前"，删除任意元素 O(1)。
// 典型用途：LRU 缓存（命中就 move_to_back，淘汰就 pop_front）、需要稳定遍历顺序的配置 / 玩家列表、先进先出的去重队列。
//
//   LinkedHashMap<uint64_t, Item> cache;
//   cache.try_emplace(id, item);
//   if (auto it = cache.find(id); it != cache.end()) cache.move_to_back(it);   // 最近使用
//   while (cache.size() > kMax) cache.pop_front();                              // 淘汰最久没用的
//
// 顺序规则：新键追加到末尾；对已有的键重复插入 / insert_or_assign / operator[] 不改变它的位置（要换位置用 move_to_back
// / move_to_front）。 迭代器是双向的（也有 rbegin / rend）。
//
// 与 FlatHashMap 的取舍：多一个 24 字节 / 元素的链接开销、遍历要沿链走，换来顺序与 O(1) 的任意位置删除。
// 迭代器 / 引用的失效规则见 detail/linked_hash_table.h 的开头：插入可能全部失效；删除只影响被删的那个、
// 以及原来最后插入的那个（它被搬到了被删元素的数组位置）。erase(iterator) 返回的"下一个"已经处理好。
// 其它限制与 FlatHashMap 相同：元素的移动构造必须 noexcept、at() 找不到键就 abort、没有自定义分配器、
// 没有哈希洪泛防护；异构查找也一样（Hash 与 KeyEqual 声明 is_transparent）。

#pragma once

#include <functional>
#include <initializer_list>
#include <tuple>
#include <type_traits>
#include <utility>

#include "base/container/detail/linked_hash_table.h"
#include "base/container/detail/slot_policy.h"

namespace breeze
{

template <typename K, typename V, typename Hash = std::hash<K>, typename KeyEqual = std::equal_to<K>>
class LinkedHashMap : public container_internal::LinkedHashTable<container_internal::MapPolicy<K, V>, Hash, KeyEqual>
{
    using Base = container_internal::LinkedHashTable<container_internal::MapPolicy<K, V>, Hash, KeyEqual>;

public:
    using key_type = K;
    using mapped_type = V;
    using typename Base::const_iterator;
    using typename Base::hasher;
    using typename Base::iterator;
    using typename Base::key_equal;
    using typename Base::size_type;
    using typename Base::value_type;

    LinkedHashMap() = default;
    explicit LinkedHashMap(size_type reserve_hint, const Hash& h = Hash(), const KeyEqual& e = KeyEqual())
        : Base(h, e)
    {
        this->reserve(reserve_hint);
    }
    LinkedHashMap(std::initializer_list<value_type> init) { insert(init); }
    template <typename It>
        requires std::input_iterator<It>
    LinkedHashMap(It first, It last)
    {
        insert(first, last);
    }
    LinkedHashMap(const LinkedHashMap&) = default;
    LinkedHashMap(LinkedHashMap&&) noexcept = default;
    LinkedHashMap& operator=(const LinkedHashMap&) = default;
    LinkedHashMap& operator=(LinkedHashMap&&) noexcept = default;
    LinkedHashMap& operator=(std::initializer_list<value_type> init)
    {
        this->clear();
        insert(init);
        return *this;
    }

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

    // 先构造临时元素再查重，键已存在时也付了构造的代价；能用 try_emplace 就用它。
    template <typename... Args>
    std::pair<iterator, bool> emplace(Args&&... args)
    {
        return this->EmplaceGeneric(std::forward<Args>(args)...);
    }

    std::pair<iterator, bool> insert(const value_type& v) { return try_emplace(v.first, v.second); }
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

    // 存在就赋值（位置不变），不存在就追加到末尾。
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

    template <typename Q = key_type>
    V& at(const typename Base::template key_arg<Q>& key)
    {
        const auto it = this->template find<Q>(key);
        if (it == this->end())
            container_internal::FatalKeyNotFound("LinkedHashMap::at");
        return it->second;
    }
    template <typename Q = key_type>
    const V& at(const typename Base::template key_arg<Q>& key) const
    {
        const auto it = this->template find<Q>(key);
        if (it == this->end())
            container_internal::FatalKeyNotFound("LinkedHashMap::at");
        return it->second;
    }

    friend void swap(LinkedHashMap& a, LinkedHashMap& b) noexcept { a.swap(b); }

    // 相等 = 键值对集合相同；顺序不参与比较（与 std::unordered_map 一致）。
    friend bool operator==(const LinkedHashMap& a, const LinkedHashMap& b)
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

template <typename K, typename V, typename H, typename E, typename Pred>
typename LinkedHashMap<K, V, H, E>::size_type erase_if(LinkedHashMap<K, V, H, E>& m, Pred pred)
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
