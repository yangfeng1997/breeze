#include "random_util.h"

#include <pthread.h>
#include <sys/random.h>
#include <unistd.h>

#include <chrono>
#include <cmath>

namespace breeze
{

namespace
{

std::uint64_t SplitMix64(std::uint64_t& state) noexcept
{
    std::uint64_t z = (state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

// 取 8 字节熵。getrandom 在启动早期熵池没就绪时也只会阻塞很短；失败（沙箱禁了 syscall）就退化成时间与地址的混合，
// 至少不同进程、不同线程不会撞同一个种子。
std::uint64_t OsEntropy() noexcept
{
    std::uint64_t v = 0;
    std::size_t got = 0;
    while (got < sizeof(v))
    {
        const ssize_t n = ::getrandom(reinterpret_cast<char*>(&v) + got, sizeof(v) - got, 0);
        if (n <= 0)
            break;
        got += static_cast<std::size_t>(n);
    }
    if (got == sizeof(v))
        return v;
    std::uint64_t mix = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    mix ^= static_cast<std::uint64_t>(::getpid()) << 32;
    mix ^= reinterpret_cast<std::uintptr_t>(&v);
    std::uint64_t st = mix;
    return SplitMix64(st);
}

// 常量初始化的线程局部标志，热路径上只读它。
thread_local bool t_seeded = false;

void OnForkChild() noexcept
{
    // 只有调用 fork 的这个线程在子进程里存活，重置它的标志即可
    t_seeded = false;
}

} // namespace

Random::Random() { Seed(OsEntropy()); }

Random::Random(const std::array<std::uint64_t, 4>& state) noexcept
    : s_(state)
{
    if ((s_[0] | s_[1] | s_[2] | s_[3]) == 0)
        Seed(0);
}

void Random::Seed(std::uint64_t seed) noexcept
{
    for (auto& w : s_)
        w = SplitMix64(seed);
}

std::uint64_t Random::Below(std::uint64_t n) noexcept
{
    if (n == 0)
        return 0;
#if defined(__SIZEOF_INT128__)
    // Lemire：乘法把 64 位随机数映射到 [0, n)，只有落在"多出来的那一小段"才需要重抽（几乎不发生，也几乎不做除法）。
    unsigned __int128 m = static_cast<unsigned __int128>((*this)()) * n;
    auto low = static_cast<std::uint64_t>(m);
    if (low < n)
    {
        const std::uint64_t threshold = (0 - n) % n; // 2^64 mod n
        while (low < threshold)
        {
            m = static_cast<unsigned __int128>((*this)()) * n;
            low = static_cast<std::uint64_t>(m);
        }
    }
    return static_cast<std::uint64_t>(m >> 64);
#else
    // 没有 128 位乘法：拒绝采样，丢掉 [2^64 - 2^64 mod n, 2^64) 这一段
    const std::uint64_t limit = std::numeric_limits<std::uint64_t>::max() - (0 - n) % n;
    std::uint64_t x;
    do
    {
        x = (*this)();
    } while (x > limit);
    return x % n;
#endif
}

double Random::Range(double min, double max) noexcept
{
    if (!(min < max))
        return min;
    const double r = min + Uniform() * (max - min);
    // 舍入可能让结果恰好等于 max（区间极窄或量级悬殊时），闭上界不属于 [min, max)
    return r < max ? r : std::nextafter(max, min);
}

float Random::Range(float min, float max) noexcept
{
    if (!(min < max))
        return min;
    const float r = min + UniformF() * (max - min);
    return r < max ? r : std::nextafter(max, min);
}

std::optional<std::size_t> Random::Weighted(std::span<const std::uint32_t> weights) noexcept
{
    std::uint64_t total = 0;
    for (const std::uint32_t w : weights)
        total += w;
    if (total == 0)
        return std::nullopt;
    std::uint64_t pick = Below(total);
    for (std::size_t i = 0; i < weights.size(); ++i)
    {
        if (pick < weights[i])
            return i;
        pick -= weights[i];
    }
    return std::nullopt; // 不会到这里：pick < total
}

std::optional<std::size_t> Random::Weighted(std::span<const double> weights) noexcept
{
    double total = 0;
    for (const double w : weights)
    {
        if (w > 0) // 负数与 NaN 都不满足，当 0
            total += w;
    }
    if (!(total > 0) || !std::isfinite(total))
        return std::nullopt;
    double pick = Uniform() * total;
    std::optional<std::size_t> last_positive;
    for (std::size_t i = 0; i < weights.size(); ++i)
    {
        if (!(weights[i] > 0))
            continue;
        last_positive = i;
        if (pick < weights[i])
            return i;
        pick -= weights[i];
    }
    return last_positive; // 累减的舍入误差让 pick 略大于余量时，落到最后一个有权重的
}

Random& ThreadRandom() noexcept
{
    thread_local Random rng{Random::UnseededTag{}}; // 常量初始化 + 平凡析构：不产生 TLS 守卫
    if (!t_seeded) [[unlikely]]
    {
        static const int once = ::pthread_atfork(nullptr, nullptr, &OnForkChild);
        (void)once;
        rng.Seed(OsEntropy());
        t_seeded = true;
    }
    return rng;
}

} // namespace breeze
