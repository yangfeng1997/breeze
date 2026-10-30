// 伪随机数：xoshiro256**（周期 2^256-1，通过 BigCrush，一次生成约 1ns），播种用 splitmix64。非密码学安全：
// 不要拿它生成 token / 密钥 / 会话 id。
//
// 两种用法：
//   1. 线程局部的全局生成器 —— 日常逻辑里的 RandomInt / RandomChance / Shuffle ...，无锁、首次使用自动用 OS 熵播种。
//   2. 显式的 Random 对象 —— 需要**可复现**的序列时用（战斗回放、测试、按种子生成地图）：同一个种子、同样的调用顺序，
//      在任何机器上得到同样的结果；State()/构造函数能把生成器存下来再恢复。线程局部那个受调度影响，做不到可复现。
//
// 有界随机数用 Lemire 的无偏方法（不是 x % n：n 不是 2 的幂时 % 有偏差）。区间一律闭区间 [min, max]，浮点一律 [min,
// max)。 min >= max 时返回 min（调用方的 bug 也不该拉倒进程）。

#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <type_traits>
#include <utility>

namespace breeze
{

class Random
{
public:
    // 满足 UniformRandomBitGenerator：能直接喂给 std::shuffle、std::normal_distribution 等标准库设施。
    using result_type = std::uint64_t;
    static constexpr result_type min() { return 0; }
    static constexpr result_type max() { return std::numeric_limits<result_type>::max(); }

    // 用 OS 熵播种（getrandom，取不到时退化成时间 + 地址混合）。
    Random();
    // 确定性：同一个种子永远同一条序列。
    explicit Random(std::uint64_t seed) noexcept { Seed(seed); }
    // 从保存的状态恢复。全零不是合法状态（会一直输出 0），按种子 0 处理。
    explicit Random(const std::array<std::uint64_t, 4>& state) noexcept;

    void Seed(std::uint64_t seed) noexcept;
    std::array<std::uint64_t, 4> State() const noexcept { return {s_[0], s_[1], s_[2], s_[3]}; }

    // 原始 64 位。
    result_type operator()() noexcept
    {
        const std::uint64_t result = Rotl(s_[1] * 5, 7) * 9;
        const std::uint64_t t = s_[1] << 17;
        s_[2] ^= s_[0];
        s_[3] ^= s_[1];
        s_[1] ^= s_[2];
        s_[0] ^= s_[3];
        s_[2] ^= t;
        s_[3] = Rotl(s_[3], 45);
        return result;
    }

    // [0, n)。n == 0 返回 0。
    std::uint64_t Below(std::uint64_t n) noexcept;

    // [min, max] 闭区间，任意整数类型（含 int64 全范围）。
    template <std::integral T>
        requires(!std::same_as<T, bool>)
    T Range(T min, T max) noexcept
    {
        if (min >= max)
            return min;
        // 全程在 T 对应的无符号类型 U 里做模运算：有符号的 max - min 会溢出，窄类型提升成 int 后再相减也会算错。
        using U = std::make_unsigned_t<T>;
        const U span_minus_1 = static_cast<U>(static_cast<U>(max) - static_cast<U>(min));
        const std::uint64_t span = static_cast<std::uint64_t>(span_minus_1) + 1; // 只有 64 位全范围时回绕成 0
        const std::uint64_t off = (span == 0) ? (*this)() : Below(span);
        return static_cast<T>(static_cast<U>(static_cast<U>(min) + static_cast<U>(off)));
    }

    // [0, 1)，53 位精度 / 24 位精度。
    double Uniform() noexcept { return static_cast<double>((*this)() >> 11) * 0x1.0p-53; }
    float UniformF() noexcept { return static_cast<float>((*this)() >> 40) * 0x1.0p-24f; }

    // [min, max)。
    double Range(double min, double max) noexcept;
    float Range(float min, float max) noexcept;

    // 以 p 的概率返回 true。p <= 0（含 NaN）永远 false，p >= 1 永远 true。
    bool Chance(double p) noexcept
    {
        if (!(p > 0.0))
            return false;
        return p >= 1.0 || Uniform() < p;
    }
    // num/den 的概率。den == 0 为 false，num >= den 为 true。精确的整数比，没有浮点误差。
    bool Chance(std::uint32_t num, std::uint32_t den) noexcept
    {
        if (den == 0)
            return false;
        return num >= den || Below(den) < num;
    }

    // Fisher-Yates 原地洗牌，每种排列等概率。
    template <std::ranges::random_access_range R>
        requires std::ranges::sized_range<R>
    void Shuffle(R&& r) noexcept
    {
        auto first = std::ranges::begin(r);
        for (auto i = static_cast<std::uint64_t>(std::ranges::size(r)); i > 1; --i)
            std::ranges::iter_swap(first + static_cast<std::ptrdiff_t>(i - 1),
                                   first + static_cast<std::ptrdiff_t>(Below(i)));
    }

    // 等概率挑一个元素，返回指针；空容器返回 nullptr（不抛、不崩）。
    template <std::ranges::forward_range R>
        requires std::ranges::sized_range<R> && std::is_lvalue_reference_v<std::ranges::range_reference_t<R>>
    auto Element(R&& r) noexcept -> std::add_pointer_t<std::ranges::range_reference_t<R>>
    {
        const auto n = static_cast<std::uint64_t>(std::ranges::size(r));
        if (n == 0)
            return nullptr;
        return &*std::ranges::next(std::ranges::begin(r), static_cast<std::ptrdiff_t>(Below(n)));
    }

    // 按权重挑下标，概率 = weights[i] / 总和。总和为 0（或空）返回 nullopt。总和在 64 位里累加，不会溢出。
    std::optional<std::size_t> Weighted(std::span<const std::uint32_t> weights) noexcept;
    // 浮点权重：负数与 NaN 当 0。
    std::optional<std::size_t> Weighted(std::span<const double> weights) noexcept;

private:
    friend Random& ThreadRandom() noexcept;

    // 未播种的占位状态：constexpr，让线程局部对象是常量初始化（访问不带 TLS 初始化守卫）。ThreadRandom
    // 首次使用前会换成真正的种子。
    struct UnseededTag
    {
    };
    constexpr explicit Random(UnseededTag) noexcept
        : s_{1, 2, 3, 4}
    {
    }

    static constexpr std::uint64_t Rotl(std::uint64_t x, int k) noexcept { return (x << k) | (x >> (64 - k)); }

    std::array<std::uint64_t, 4> s_{};
};

// 当前线程的生成器。首次使用时用 OS 熵播种；fork 之后子进程里的下一次使用会重新播种
// （否则父子进程会吐出一模一样的序列）。要确定性就调 ThreadRandom().Seed(x)。
Random& ThreadRandom() noexcept;

inline void SetSeed(std::uint64_t seed) noexcept { ThreadRandom().Seed(seed); }

// 线程局部生成器的便捷封装。
template <std::integral T>
    requires(!std::same_as<T, bool>)
T RandomInt(T min, T max) noexcept
{
    return ThreadRandom().Range(min, max);
}
inline double RandomDouble(double min, double max) noexcept { return ThreadRandom().Range(min, max); }
inline bool RandomChance(double p) noexcept { return ThreadRandom().Chance(p); }
inline bool RandomChance(std::uint32_t num, std::uint32_t den) noexcept { return ThreadRandom().Chance(num, den); }
template <typename R>
void Shuffle(R&& r) noexcept
{
    ThreadRandom().Shuffle(std::forward<R>(r));
}
template <typename R>
auto RandomElement(R&& r) noexcept
{
    return ThreadRandom().Element(std::forward<R>(r));
}
inline std::optional<std::size_t> RandomWeighted(std::span<const std::uint32_t> w) noexcept
{
    return ThreadRandom().Weighted(w);
}

// 无状态的适配器：每次调用转给当前线程的生成器，可放进 std::shuffle / 分布等标准库设施。
struct ThreadLocalUrbg
{
    using result_type = std::uint64_t;
    static constexpr result_type min() { return 0; }
    static constexpr result_type max() { return std::numeric_limits<result_type>::max(); }
    result_type operator()() noexcept { return ThreadRandom()(); }
};

} // namespace breeze
