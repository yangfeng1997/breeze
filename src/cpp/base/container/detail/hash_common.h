// 哈希容器共用的小件：透明查找的判定与参数类型、哈希搅拌、"找不到键"的终止。

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <type_traits>

namespace breeze::container_internal
{

[[noreturn]] inline void FatalKeyNotFound(const char* what)
{
    std::fprintf(stderr, "%s: key not found\n", what);
    std::abort();
}

// Hash / KeyEqual 都声明了 is_transparent 才允许用别的类型的键查找（如 string_view 查 std::string 的表，不构造临时
// string）。
template <typename T, typename = void>
struct HasIsTransparent : std::false_type
{
};
template <typename T>
struct HasIsTransparent<T, std::void_t<typename T::is_transparent>> : std::true_type
{
};

// key_arg<K>：透明时就是 K（能从实参推导出来），否则固定为 key_type（K 取默认值，别的类型靠隐式转换）。
// 必须直接别名成 K 本身：经过 std::conditional_t 的话 K 属于非推导上下文，永远推导不出来。
template <bool Transparent>
struct KeyArg
{
    template <typename K, typename Key>
    using type = K;
};
template <>
struct KeyArg<false>
{
    template <typename K, typename Key>
    using type = Key;
};

// 把哈希值再搅一遍：std::hash<int> 是恒等函数，直接取低位 / 高位会让规律性的键（2 的幂的倍数、连续 id）扎堆。
// 折叠乘法（128 位乘积的高低两半异或）：一条乘法指令，雪崩效果足够好。
inline std::size_t MixHash(std::size_t h) noexcept
{
    const unsigned __int128 m = static_cast<unsigned __int128>(h ^ 0x2545F4914F6CDD1DULL) * 0x9ddfea08eb382d69ULL;
    return static_cast<std::size_t>(static_cast<std::uint64_t>(m) ^ static_cast<std::uint64_t>(m >> 64));
}

} // namespace breeze::container_internal
