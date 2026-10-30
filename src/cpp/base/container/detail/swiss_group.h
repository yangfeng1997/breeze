// Swiss table 的控制字节与"一组"的并行匹配：SSE2（x86-64 基线指令集）与可移植的 SWAR（8 字节一组）两种实现。
//
// 控制字节（ctrl_t，有符号）：满 = H2 (0..127，哈希的低 7 位)；空 = -128；已删除（墓碑）= -2。
// 所以"满"恰好是非负数，"空或已删除"恰好是负数，一次比较就能分开。
//
// 只支持小端：SWAR 的位序与槽序对应关系依赖它。
// 编译时定义 BREEZE_CONTAINER_FORCE_SWAR 可以在 x86 上强制用 SWAR，给没有 SSE2 的平台（如 aarch64）的路径做测试。
// 一个程序里所有翻译单元要一致（Group 的定义随这个宏变，混用违反 ODR）：只在专门的测试可执行文件上定义它。

#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__SSE2__) && !defined(BREEZE_CONTAINER_FORCE_SWAR)
#include <emmintrin.h>
#define BREEZE_CONTAINER_HAS_SSE2 1
#endif

namespace breeze::container_internal
{

static_assert(std::endian::native == std::endian::little, "swiss_group.h 只支持小端平台");

using ctrl_t = std::int8_t;
inline constexpr ctrl_t kEmpty = -128;
inline constexpr ctrl_t kDeleted = -2;

inline constexpr bool IsFull(ctrl_t c) noexcept { return c >= 0; }
inline constexpr bool IsEmpty(ctrl_t c) noexcept { return c == kEmpty; }
inline constexpr bool IsDeleted(ctrl_t c) noexcept { return c == kDeleted; }

// 一次匹配的结果：哪几个槽命中。SignificantBits = 一组有几个槽，Shift = 每个槽在掩码里占几位的 log2。
template <typename T, int SignificantBits, int Shift = 0>
class BitMask
{
public:
    explicit BitMask(T mask) noexcept
        : mask_(mask)
    {
    }

    BitMask& operator++() noexcept
    {
        mask_ &= (mask_ - 1); // 清掉最低的那个置位
        return *this;
    }
    explicit operator bool() const noexcept { return mask_ != 0; }
    std::uint32_t operator*() const noexcept { return LowestBitSet(); }
    BitMask begin() const noexcept { return *this; }
    BitMask end() const noexcept { return BitMask(0); }
    friend bool operator==(const BitMask& a, const BitMask& b) noexcept { return a.mask_ == b.mask_; }

    // 以下都要求掩码非空。
    std::uint32_t LowestBitSet() const noexcept { return static_cast<std::uint32_t>(std::countr_zero(mask_)) >> Shift; }
    // 低位方向连续的"未命中"槽数（掩码为 0 时 = 一组的槽数）。
    std::uint32_t TrailingZeros() const noexcept
    {
        return static_cast<std::uint32_t>(std::countr_zero(mask_)) >> Shift;
    }
    // 高位方向连续的"未命中"槽数。
    std::uint32_t LeadingZeros() const noexcept
    {
        constexpr int total_bits = SignificantBits << Shift;
        constexpr int extra_bits = static_cast<int>(sizeof(T) * 8) - total_bits;
        return static_cast<std::uint32_t>(std::countl_zero(static_cast<T>(mask_ << extra_bits))) >> Shift;
    }

private:
    T mask_;
};

#ifdef BREEZE_CONTAINER_HAS_SSE2

struct Group
{
    static constexpr std::size_t kWidth = 16;
    using Mask = BitMask<std::uint16_t, kWidth>;

    explicit Group(const ctrl_t* pos) noexcept
        : ctrl(_mm_loadu_si128(reinterpret_cast<const __m128i*>(pos)))
    {
    }

    Mask Match(ctrl_t h2) const noexcept
    {
        return Mask(static_cast<std::uint16_t>(_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_set1_epi8(h2), ctrl))));
    }
    Mask MatchEmpty() const noexcept { return Match(kEmpty); }
    Mask MatchEmptyOrDeleted() const noexcept
    {
        return Mask(static_cast<std::uint16_t>(_mm_movemask_epi8(_mm_cmpgt_epi8(_mm_setzero_si128(), ctrl))));
    }
    Mask MatchFull() const noexcept
    {
        return Mask(static_cast<std::uint16_t>(_mm_movemask_epi8(_mm_cmpgt_epi8(ctrl, _mm_set1_epi8(-1)))));
    }

    __m128i ctrl;
};

#else

struct Group
{
    static constexpr std::size_t kWidth = 8;
    using Mask = BitMask<std::uint64_t, kWidth, 3>; // 每个槽占 8 位，命中时置该字节的最高位

    explicit Group(const ctrl_t* pos) noexcept { std::memcpy(&ctrl, pos, sizeof(ctrl)); }

    // 经典的"字节等于 h2"：先异或，再找零字节。零字节的上方若紧挨着值为 1 的字节会有假阳性
    // （即控制字节 = h2 ^ 1，是另一个满槽的 H2），所以调用方必须再用 key_equal 确认；不会漏报。
    Mask Match(ctrl_t h2) const noexcept
    {
        const std::uint64_t x = ctrl ^ (kLsbs * static_cast<std::uint8_t>(h2));
        return Mask((x - kLsbs) & ~x & kMsbs);
    }
    // 空 = 0x80：最高位为 1 且 bit1 为 0。墓碑 0xFE 的 bit1 为 1，满槽的最高位为 0。精确，无假阳性。
    // （左移 6 位时相邻字节只会污染本字节的低 6 位，不影响最高位。）
    Mask MatchEmpty() const noexcept { return Mask((ctrl & ~(ctrl << 6)) & kMsbs); }
    Mask MatchEmptyOrDeleted() const noexcept { return Mask(ctrl & kMsbs); }
    Mask MatchFull() const noexcept { return Mask(~ctrl & kMsbs); }

    static constexpr std::uint64_t kMsbs = 0x8080808080808080ULL;
    static constexpr std::uint64_t kLsbs = 0x0101010101010101ULL;
    std::uint64_t ctrl;
};

#endif

inline constexpr std::size_t kGroupWidth = Group::kWidth;

} // namespace breeze::container_internal
