// LinkedHashSet：哈希集合 + 按插入顺序遍历（语义、取舍、失效规则见 linked_hash_map.h）。
//
//   LinkedHashSet<uint64_t> seen;               // 去重并记住先来后到
//   if (seen.insert(id).second) queue_order...  // 新来的才返回 true
//   seen.pop_front();                           // 去掉最早的
//
// 对已有元素重复插入不改变它的位置（要换位置用 move_to_back / move_to_front）。元素是 const 的。

#pragma once

#include <functional>
#include <initializer_list>
#include <type_traits>
#include <utility>

#include "detail/linked_hash_table.h"
#include "detail/slot_policy.h"

namespace breeze
{

template <typename K, typename Hash = std::hash<K>, typename KeyEqual = std::equal_to<K>>
class LinkedHashSet : public container_internal::LinkedHashTable<container_internal::SetPolicy<K>, Hash, KeyEqual>
{
    using Base = container_internal::LinkedHashTable<container_internal::SetPolicy<K>, Hash, KeyEqual>;

public:
    using key_type = K;
    using typename Base::const_iterator;
    using typename Base::hasher;
    using typename Base::iterator;
    using typename Base::key_equal;
    using typename Base::size_type;
    using typename Base::value_type;

    LinkedHashSet() = default;
    explicit LinkedHashSet(size_type reserve_hint, const Hash& h = Hash(), const KeyEqual& e = KeyEqual())
        : Base(h, e)
    {
        this->reserve(reserve_hint);
    }
    LinkedHashSet(std::initializer_list<value_type> init) { insert(init); }
    template <typename It>
        requires std::input_iterator<It>
    LinkedHashSet(It first, It last)
    {
        insert(first, last);
    }
    LinkedHashSet(const LinkedHashSet&) = default;
    LinkedHashSet(LinkedHashSet&&) noexcept = default;
    LinkedHashSet& operator=(const LinkedHashSet&) = default;
    LinkedHashSet& operator=(LinkedHashSet&&) noexcept = default;
    LinkedHashSet& operator=(std::initializer_list<value_type> init)
    {
        this->clear();
        insert(init);
        return *this;
    }

    // 不存在才追加到末尾；存在时 key 原封不动（右值不会被移走）。
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

    template <typename... Args>
    std::pair<iterator, bool> emplace(Args&&... args)
    {
        return this->EmplaceGeneric(std::forward<Args>(args)...);
    }

    friend void swap(LinkedHashSet& a, LinkedHashSet& b) noexcept { a.swap(b); }

    friend bool operator==(const LinkedHashSet& a, const LinkedHashSet& b)
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
typename LinkedHashSet<K, H, E>::size_type erase_if(LinkedHashSet<K, H, E>& s, Pred pred)
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
