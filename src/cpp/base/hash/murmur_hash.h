// MurmurHash64A（MurmurHash2 的 64 位版，Austin Appleby，公有领域）：快、分布好、**结果稳定**的字节哈希。
//
//   const std::size_t node = MurmurHash64A(player_name) % node_count;    // 文本 / 字节串（string_view），种子默认 0
//   const std::uint64_t h = MurmurHash64ABytes(&id, sizeof(id));         // 裸指针 + 长度
//   const std::uint64_t h2 = MurmurHash64A(player_name, MurmurSeed{42}); // 指定种子
//
// 什么时候用它而不是 std::hash：std::hash<string> 的结果依赖标准库实现与版本，进程重启、换编译器、跨语言（Go 服务）
// 就变了；凡是要"同一个键在任何进程、任何机器、任何语言里都落到同一个地方"的（分片、路由、一致性哈希、持久化的桶号）
// 都用它。哈希容器内部不需要它（FlatHashMap 自己有搅拌，且不要求稳定）。
//
// 结果与平台无关：按小端读 8 字节块，大端平台上会先换字节序；读取走 memcpy，不要求对齐、不违反严格别名。
// 与参考实现（smhasher 的 MurmurHash2.cpp 里的 MurmurHash64A）逐位一致：smhasher 的验证值是 0x1F0D3804
// （murmur_hash_test 里检查）。非密码学哈希：别拿它做签名 / 校验和防篡改，也别在键由攻击者控制的地方当唯一的防线。
//
// 接口上的两处刻意设计（原版的 `MurmurHash64A(const void*, int len, uint64_t seed)` 与
// `MurmurHash64A(const std::string&, uint64_t seed)` 两个重载放在一起有陷阱）：
//   - 种子是强类型 MurmurSeed，不是裸整数：否则 `MurmurHash64A("hello", 42)` 会因为 (const void*, size_t) 重载更匹配，
//     把 42 当成长度，从 6 字节的字面量里读 42 字节（第一版就这样写错了，被 asan 查出来）。现在整数不能隐式变成种子，
//     这种写法直接编译不过。
//   - 裸指针 + 长度的版本叫 MurmurHash64ABytes，不和 string_view 版本重载：`MurmurHash64A(buf, n)`（旧写法）会
//     编译失败，而不是悄悄把长度当种子。
//
// 其它语言要对拍时的测试向量（seed = 0）：""→0，"a"→0x071717d2d36b6b11，"hello"→0x1e68d17c457bf117，
// "hello world"→0xd3ba2368a832afce；("hello", seed 42)→0xd417125ccb971887。

#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace breeze
{

// 强类型的种子：整数不能隐式转成它，见文件头的说明。
struct MurmurSeed
{
    constexpr MurmurSeed() noexcept = default;
    constexpr explicit MurmurSeed(std::uint64_t v) noexcept
        : value(v)
    {
    }
    std::uint64_t value = 0;
};

inline std::uint64_t MurmurHash64ABytes(const void* key, std::size_t len, MurmurSeed seed_value = {}) noexcept
{
    const std::uint64_t seed = seed_value.value;
    constexpr std::uint64_t m = 0xc6a4a7935bd1e995ULL;
    constexpr int r = 47;

    std::uint64_t h = seed ^ (static_cast<std::uint64_t>(len) * m);

    const auto* p = static_cast<const unsigned char*>(key);
    const unsigned char* const end = p + (len / 8) * 8;
    for (; p != end; p += 8)
    {
        std::uint64_t k;
        std::memcpy(&k, p, sizeof(k));
        if constexpr (std::endian::native == std::endian::big)
            k = std::byteswap(k); // 统一按小端解释，结果与平台无关
        k *= m;
        k ^= k >> r;
        k *= m;
        h ^= k;
        h *= m;
    }

    // 不足 8 字节的尾巴：第 i 个字节放到第 i 个字节位（小端），不管 len 是多少都不越界
    const std::size_t rem = len & 7;
    if (rem != 0)
    {
        std::uint64_t tail = 0;
        for (std::size_t i = 0; i < rem; ++i)
            tail |= static_cast<std::uint64_t>(p[i]) << (8 * i);
        h ^= tail;
        h *= m;
    }

    h ^= h >> r;
    h *= m;
    h ^= h >> r;
    return h;
}

inline std::uint64_t MurmurHash64A(std::string_view s, MurmurSeed seed = {}) noexcept
{
    return MurmurHash64ABytes(s.data(), s.size(), seed);
}

} // namespace breeze
