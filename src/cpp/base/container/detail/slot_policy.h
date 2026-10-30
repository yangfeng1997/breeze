// 哈希容器的"槽策略"：槽里实际存什么、怎么构造 / 拷贝 / 搬动 / 销毁。FlatHash* 与 LinkedHash* 共用。
//
// 必须提供：key_type / value_type（对外暴露的元素类型）/ slot_type（实际存的）/ kNothrowTransfer、
//   key(slot) / element(slot)、construct(slot*, args...) / construct_copy(dst, src) / destroy(slot*) / transfer(dst,
//   src)
// transfer 在 dst 上移动构造并销毁 src，不许抛（容器搬动元素时没法回滚）。

#pragma once

#include <type_traits>
#include <utility>

namespace breeze::container_internal
{

// 槽是 {const K, V} 与 {K, V} 的联合：对外暴露带 const 键的 pair（键不能被改），搬动槽时把键当可移动的用。
// 两种 pair 布局相同，靠联合在 GCC / Clang 上定义良好的类型双关读写（Abseil 同样的做法）。
template <typename K, typename V>
struct MapPolicy
{
    using key_type = K;
    using mapped_type = V;
    using value_type = std::pair<const K, V>;
    using mutable_value_type = std::pair<K, V>;

    static constexpr bool kNothrowTransfer =
        std::is_nothrow_move_constructible_v<K> && std::is_nothrow_move_constructible_v<V>;

    union slot_type
    {
        slot_type() {}
        ~slot_type() {}
        value_type value;
        mutable_value_type mutable_value;
    };
    static_assert(sizeof(value_type) == sizeof(mutable_value_type) &&
                      alignof(value_type) == alignof(mutable_value_type),
                  "pair<const K, V> 与 pair<K, V> 布局必须一致");

    static const K& key(const slot_type& s) noexcept { return s.value.first; }
    static value_type& element(slot_type& s) noexcept { return s.value; }
    static const value_type& element(const slot_type& s) noexcept { return s.value; }

    template <typename... Args>
    static void construct(slot_type* s, Args&&... args)
    {
        ::new (static_cast<void*>(&s->value)) value_type(std::forward<Args>(args)...);
    }
    static void construct_copy(slot_type* dst, const slot_type& src)
    {
        ::new (static_cast<void*>(&dst->value)) value_type(src.value);
    }
    static void destroy(slot_type* s) noexcept { s->value.~value_type(); }
    static void transfer(slot_type* dst, slot_type* src) noexcept
    {
        ::new (static_cast<void*>(&dst->mutable_value)) mutable_value_type(std::move(src->mutable_value));
        src->mutable_value.~mutable_value_type();
    }
};

template <typename K>
struct SetPolicy
{
    using key_type = K;
    using value_type = K;
    using slot_type = K;

    static constexpr bool kNothrowTransfer = std::is_nothrow_move_constructible_v<K>;

    static const K& key(const slot_type& s) noexcept { return s; }
    // 集合的元素对外永远是 const：改了键就破坏了哈希位置。
    static const value_type& element(slot_type& s) noexcept { return s; }
    static const value_type& element(const slot_type& s) noexcept { return s; }

    template <typename... Args>
    static void construct(slot_type* s, Args&&... args)
    {
        ::new (static_cast<void*>(s)) K(std::forward<Args>(args)...);
    }
    static void construct_copy(slot_type* dst, const slot_type& src) { ::new (static_cast<void*>(dst)) K(src); }
    static void destroy(slot_type* s) noexcept { s->~K(); }
    static void transfer(slot_type* dst, slot_type* src) noexcept
    {
        ::new (static_cast<void*>(dst)) K(std::move(*src));
        src->~K();
    }
};

} // namespace breeze::container_internal
