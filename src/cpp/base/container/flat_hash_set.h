// FlatHashSet：开放寻址的 Swiss table 哈希集合。语义、限制与异构查找见 flat_hash_map.h 的说明
// （插入可能让迭代器 / 引用失效、遍历顺序不确定、元素的移动构造必须 noexcept、at 之类找不到就 abort）。
//
//   FlatHashSet<uint64_t> online;
//   online.insert(id);  online.contains(id);  online.erase(id);

#pragma once

#include <functional>
#include <initializer_list>
#include <type_traits>
#include <utility>

#include "base/container/detail/raw_hash_table.h"
#include "base/container/detail/slot_policy.h"

namespace breeze
{

template <typename K, typename Hash = std::hash<K>, typename KeyEqual = std::equal_to<K>>
class FlatHashSet : public container_internal::RawHashTable<container_internal::SetPolicy<K>, Hash, KeyEqual>
{
    using Base = container_internal::RawHashTable<container_internal::SetPolicy<K>, Hash, KeyEqual>;

public:
    using key_type = K;
    using typename Base::const_iterator;
    using typename Base::hasher;
    using typename Base::iterator;
    using typename Base::key_equal;
    using typename Base::size_type;
    using typename Base::value_type;

    FlatHashSet() = default;
    explicit FlatHashSet(size_type reserve_hint, const Hash& h = Hash(), const KeyEqual& e = KeyEqual())
        : Base(h, e)
    {
        this->reserve(reserve_hint);
    }
    FlatHashSet(std::initializer_list<value_type> init) { insert(init); }
    template <typename It>
        requires std::input_iterator<It>
    FlatHashSet(It first, It last)
    {
        insert(first, last);
    }
    FlatHashSet(const FlatHashSet&) = default;
    FlatHashSet(FlatHashSet&&) noexcept = default;
    FlatHashSet& operator=(const FlatHashSet&) = default;
    FlatHashSet& operator=(FlatHashSet&&) noexcept = default;
    FlatHashSet& operator=(std::initializer_list<value_type> init)
    {
        this->clear();
        insert(init);
        return *this;
    }

    // 不存在才插入；存在时 key 原封不动（右值不会被移走）。返回 (迭代器, 是否新插入)。
    std::pair<iterator, bool> insert(const key_type& key) { return this->FindOrConstruct(key, key); }
    std::pair<iterator, bool> insert(key_type&& key) { return this->FindOrConstruct(key, std::move(key)); }
    template <typename It>
        requires std::input_iterator<It>
    void insert(It first, It last)
    {
        for (; first != last; ++first)
            insert(*first);
    }
    void insert(std::initializer_list<value_type> init) { insert(init.begin(), init.end()); }

    // 先构造临时元素再查重；键已存在时也付了一次构造的代价。
    template <typename... Args>
    std::pair<iterator, bool> emplace(Args&&... args)
    {
        return this->EmplaceGeneric(std::forward<Args>(args)...);
    }

    friend void swap(FlatHashSet& a, FlatHashSet& b) noexcept { a.swap(b); }

    friend bool operator==(const FlatHashSet& a, const FlatHashSet& b)
    {
        if (a.size() != b.size())
            return false;
        for (const auto& k : a)
        {
            if (!b.contains(k))
                return false;
        }
        return true;
    }
};

template <typename K, typename H, typename E, typename Pred>
typename FlatHashSet<K, H, E>::size_type erase_if(FlatHashSet<K, H, E>& s, Pred pred)
{
    const auto before = s.size();
    for (auto it = s.begin(); it != s.end();)
    {
        if (pred(*it))
            it = s.erase(it);
        else
            ++it;
    }
    return before - s.size();
}

} // namespace breeze
