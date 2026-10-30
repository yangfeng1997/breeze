// Random / ThreadRandom：
//   - 已知答案：xoshiro256** 与 splitmix64、Lemire 有界采样（含重抽分支）、Range / Uniform，期望值由独立的 Python
//   参考实现算出
//     （状态 {1,2,3,4} 的前几个输出与 xoshiro 作者公布的测试向量一致）。
//   - 统计：固定种子下的卡方 / 偏差检验 —— 用的是固定种子，结果是确定的，不会偶发失败；阈值放得很宽，只拦"明显有偏"。
//   - 线程局部：不同线程种子不同、SetSeed 可复现、fork 之后子进程重新播种。
// 无测试框架依赖：失败即 abort。

#include "random_util.h"

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <list>
#include <map>
#include <random>
#include <set>
#include <thread>
#include <vector>

using namespace breeze;

#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                              \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (0)

namespace
{

static_assert(std::uniform_random_bit_generator<Random>);
static_assert(std::uniform_random_bit_generator<ThreadLocalUrbg>);

// 卡方统计量；df = bins - 1。阈值取 均值 + 8 倍标准差：对"明显有偏"足够灵敏，固定种子下也不会碰运气。
double ChiSquare(const std::vector<long>& observed, double expected_each)
{
    double stat = 0;
    for (const long o : observed)
    {
        const double d = static_cast<double>(o) - expected_each;
        stat += d * d / expected_each;
    }
    return stat;
}

bool ChiSquareOk(const std::vector<long>& observed, double expected_each)
{
    const double df = static_cast<double>(observed.size() - 1);
    return ChiSquare(observed, expected_each) < df + 8.0 * std::sqrt(2.0 * df);
}

bool ChiSquareOkWeighted(const std::vector<long>& observed, const std::vector<double>& expected)
{
    double stat = 0;
    for (std::size_t i = 0; i < observed.size(); ++i)
    {
        const double d = static_cast<double>(observed[i]) - expected[i];
        stat += d * d / expected[i];
    }
    const double df = static_cast<double>(observed.size() - 1);
    return stat < df + 8.0 * std::sqrt(2.0 * df);
}

void TestKnownAnswers()
{
    // Seed(0)：splitmix64(0) 的前 4 个输出
    Random z(0);
    const std::array<std::uint64_t, 4> st0{
        0xe220a8397b1dcdafULL, 0x6e789e6aa1b965f4ULL, 0x06c45d188009454fULL, 0xf88bb8a8724c81ecULL};
    CHECK(z.State() == st0);

    // 状态 {1,2,3,4}：xoshiro256** 公布的测试向量（前四个是 11520、0、1509978240、1215971899390074240）
    Random a(std::array<std::uint64_t, 4>{1, 2, 3, 4});
    const std::uint64_t expect[] = {0x0000000000002d00ULL,
                                    0x0000000000000000ULL,
                                    0x000000005a007080ULL,
                                    0x10e0000000009d80ULL,
                                    0x10e0b61ce1009d80ULL,
                                    0x0870021ce143ad00ULL,
                                    0xe071c3c2e143f089ULL,
                                    0x75a1690ef7a20380ULL,
                                    0x9309685b465c23f9ULL,
                                    0x284f3cc2e13e3c88ULL};
    for (const auto e : expect)
        CHECK(a() == e);

    Random b(12345);
    const std::uint64_t expect2[] = {0xbe6a36374160d49bULL,
                                     0x214aaa0637a688c6ULL,
                                     0xf69d16de9954d388ULL,
                                     0x0c60048c4e96e033ULL,
                                     0x8e2076aeed51c648ULL,
                                     0x02bbcc1c1fc50f84ULL,
                                     0x28e72a4fec84f699ULL,
                                     0x4bb9d7cbb8dddebeULL};
    for (const auto e : expect2)
        CHECK(b() == e);
}

void TestBelowKnownAnswers()
{
    // n = ceil(2/3 * 2^64)：2^64 mod n = 0x5555…5555，约三分之一的抽样会走"拒绝重抽"分支，40 个里有 13 次重抽。
    // 40 个结果与参考实现逐个相同，说明重抽的判定与次数都对（少抽一次后面整条序列都会错位）。
    // 不要用 3*2^62 这类"低位全零"的 n：x*n 的低 64 位只有 4 种取值，判定阈值错成别的数结果照样一样。
    Random r(777);
    const std::uint64_t n = 0xAAAAAAAAAAAAAAABULL;
    const std::uint64_t expect[] = {
        0xa0c9f8adceaa6b70ULL, 0x6cc7c2f82281bd10ULL, 0xa733a74a2c9832abULL, 0x2a37654b5c39622aULL,
        0x4bdb343d11ecd564ULL, 0x91fbe892e10138e7ULL, 0x5abd04e35855503dULL, 0x23896e32dc9ee443ULL,
        0x23c8781f69c3a54dULL, 0x53408399801413a6ULL, 0x0673783ca35d3342ULL, 0x07796bb57f936e2eULL,
        0xa375e86d18f4574cULL, 0x28334590a791e8eaULL, 0x126c5dbd52acfedfULL, 0x5c7f9acda6bead24ULL,
        0xa7ebeae401e89729ULL, 0x2696bca837b637b6ULL, 0x62d083f9c32ab255ULL, 0x6e4e0b70ff7b2eddULL,
        0x3b7d5cc1a11b8479ULL, 0x31a3e42f53d10592ULL, 0x8ec545187c582b71ULL, 0x2a630ff4f5fbe18bULL,
        0x51952b56736c8b0bULL, 0x3a8fdd9dca277021ULL, 0x8fd4466f442bdf96ULL, 0x8da1c201ea34da31ULL,
        0x81a1828642bc0b06ULL, 0x4b59e14352fa679aULL, 0x6c7233d34afc17d8ULL, 0x5525202a3aaf04f3ULL,
        0x81f00f2f1fb71045ULL, 0x0329ea1ae29027c3ULL, 0x96b0c3b53f3d0f81ULL, 0x4d13240cffa3fdc1ULL,
        0x19d37ba1a14ef967ULL, 0x7e058336e6d8e90dULL, 0x2e45b22563a81f80ULL, 0x30628aa860ef3712ULL};
    for (const auto e : expect)
        CHECK(r.Below(n) == e);

    Random r2(99);
    const int e2[] = {348, 564, 378, 855, 785, 205, 277, 58,  846, 584,
                      740, 172, 149, 125, 395, 514, 196, 907, 150, 754};
    for (const int e : e2)
        CHECK(r2.Below(1000) == static_cast<std::uint64_t>(e));

    Random r3(5);
    const int e3[] = {2, 4, 4, 5, 3, 5, 3, 5, 2, 2, 6, 1, 2, 5, 3, 6, 4, 5, 3, 5};
    for (const int e : e3)
        CHECK(r3.Below(7) == static_cast<std::uint64_t>(e));
}

void TestRangeAndUniformKnownAnswers()
{
    Random a(2024);
    const int e8[] = {-114, 72, -110, -88, 70, -66, -28, -63, 14, -116};
    for (const int e : e8)
        CHECK(a.Range<std::int8_t>(-128, 127) == e);

    Random b(2024);
    const int e5[] = {-5, 3, -5, -4, 3, -3, -1, -3, 1, -5};
    for (const int e : e5)
        CHECK(b.Range(-5, 5) == e);

    Random c(1);
    CHECK(c.Uniform() == 0.7029218331588505);
    CHECK(c.Uniform() == 0.5204366199388569);
    CHECK(c.Uniform() == 0.5741057000197225);
    CHECK(c.Uniform() == 0.39132860204190445);
}

// n 不是 2 的幂时 x % n 有偏差。n = 2/3 * 2^64 时偏差最明显：[0, n/2) 被取模盖了两遍，会占 2/3 而不是 1/2。
void TestBelowIsUnbiased()
{
    Random r(42);
    const std::uint64_t n = 12297829382473034410ULL; // 向下取整的 2/3 * 2^64
    constexpr long kN = 200000;
    long low_half = 0;
    for (long i = 0; i < kN; ++i)
        low_half += r.Below(n) < n / 2 ? 1 : 0;
    const double sigma = std::sqrt(kN * 0.25);
    CHECK(std::abs(static_cast<double>(low_half) - kN * 0.5) < 6 * sigma);
}

void TestBelowUniform()
{
    Random r(7);
    for (const std::uint64_t n : {2ULL, 3ULL, 7ULL, 10ULL, 100ULL, 255ULL, 256ULL, 1000ULL})
    {
        const long samples = static_cast<long>(n) * 2000;
        std::vector<long> bins(n, 0);
        for (long i = 0; i < samples; ++i)
        {
            const std::uint64_t v = r.Below(n);
            CHECK(v < n);
            ++bins[v];
        }
        CHECK(ChiSquareOk(bins, 2000.0));
    }
    CHECK(r.Below(0) == 0);
    CHECK(r.Below(1) == 0);
}

void TestRangeIntegers()
{
    Random r(11);
    // 闭区间：两端都取得到，且不越界
    {
        std::set<int> seen;
        for (int i = 0; i < 5000; ++i)
        {
            const int v = r.Range(3, 8);
            CHECK(v >= 3 && v <= 8);
            seen.insert(v);
        }
        CHECK(seen.size() == 6);
    }
    // 窄类型的全范围：有符号 max - min 在 int 里提升后会算错，这里逐个值都要出现，分布均匀
    {
        std::vector<long> bins(256, 0);
        for (int i = 0; i < 256 * 1000; ++i)
            ++bins[static_cast<std::size_t>(static_cast<int>(r.Range<std::int8_t>(-128, 127)) + 128)];
        CHECK(ChiSquareOk(bins, 1000.0));
    }
    {
        std::vector<long> bins(256, 0);
        for (int i = 0; i < 256 * 1000; ++i)
            ++bins[r.Range<std::uint8_t>(0, 255)];
        CHECK(ChiSquareOk(bins, 1000.0));
    }
    {
        std::vector<long> bins(11, 0);
        for (int i = 0; i < 11 * 2000; ++i)
            ++bins[static_cast<std::size_t>(r.Range<std::int16_t>(-5, 5) + 5)];
        CHECK(ChiSquareOk(bins, 2000.0));
    }
    // 跨 0 的有符号区间、全范围
    for (int i = 0; i < 2000; ++i)
    {
        const int v = r.Range(-1000, 1000);
        CHECK(v >= -1000 && v <= 1000);
        const std::int64_t w = r.Range<std::int64_t>(-5, 5);
        CHECK(w >= -5 && w <= 5);
    }
    {
        // 全范围：跨度 2^64 回绕成 0，走"直接取原始 64 位"分支；高位两种取值都应出现
        long neg = 0;
        for (int i = 0; i < 2000; ++i)
            neg += r.Range(std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max()) < 0;
        CHECK(neg > 800 && neg < 1200);
        std::uint64_t acc = 0;
        for (int i = 0; i < 100; ++i)
            acc |= r.Range<std::uint64_t>(0, std::numeric_limits<std::uint64_t>::max());
        CHECK(acc > (1ULL << 63));
    }
    {
        long neg = 0;
        for (int i = 0; i < 2000; ++i)
            neg += r.Range(std::numeric_limits<int>::min(), std::numeric_limits<int>::max()) < 0;
        CHECK(neg > 800 && neg < 1200);
    }
    // 退化：min == max、min > max 都返回 min
    CHECK(r.Range(5, 5) == 5);
    CHECK(r.Range(9, 3) == 9);
    CHECK(r.Range(-3, -3) == -3);
    CHECK(r.Range<std::uint32_t>(10, 2) == 10u);
    CHECK(r.Range<std::int8_t>(127, 127) == 127);
}

void TestFloating()
{
    Random r(3);
    double sum = 0;
    for (int i = 0; i < 100000; ++i)
    {
        const double u = r.Uniform();
        CHECK(u >= 0.0 && u < 1.0);
        sum += u;
        const float f = r.UniformF();
        CHECK(f >= 0.0f && f < 1.0f);
    }
    CHECK(std::abs(sum / 100000 - 0.5) < 0.01);

    for (int i = 0; i < 10000; ++i)
    {
        const double v = r.Range(-2.5, 7.5);
        CHECK(v >= -2.5 && v < 7.5);
        const float w = r.Range(-1.0f, 1.0f);
        CHECK(w >= -1.0f && w < 1.0f);
    }
    // 区间窄到只容得下一个值：舍入不能把上界（开区间）吐出来
    const double hi = std::nextafter(1.0, 2.0);
    for (int i = 0; i < 10000; ++i)
        CHECK(r.Range(1.0, hi) == 1.0);
    const float hif = std::nextafter(1.0f, 2.0f);
    for (int i = 0; i < 10000; ++i)
        CHECK(r.Range(1.0f, hif) == 1.0f);
    // 量级悬殊：min 很大、跨度相对很小，r * span 加到 min 上会舍入成 max
    for (int i = 0; i < 10000; ++i)
    {
        const double v = r.Range(1e16, 1e16 + 4.0);
        CHECK(v >= 1e16 && v < 1e16 + 4.0);
    }
    // 退化
    CHECK(r.Range(2.0, 2.0) == 2.0);
    CHECK(r.Range(3.0, 1.0) == 3.0);
    CHECK(r.Range(2.0f, 2.0f) == 2.0f);
    CHECK(std::isnan(r.Range(std::nan(""), 1.0)));
}

void TestChance()
{
    Random r(5);
    for (int i = 0; i < 10000; ++i)
    {
        CHECK(!r.Chance(0.0));
        CHECK(r.Chance(1.0));
        CHECK(!r.Chance(-0.5));
        CHECK(r.Chance(1.5));
        CHECK(!r.Chance(std::nan("")));
        CHECK(!r.Chance(0u, 10u));
        CHECK(r.Chance(10u, 10u));
        CHECK(r.Chance(11u, 10u));
        CHECK(!r.Chance(5u, 0u)); // 分母 0
        CHECK(!r.Chance(0u, 0u));
    }
    constexpr long kN = 100000;
    long hit = 0, hit2 = 0;
    for (long i = 0; i < kN; ++i)
    {
        hit += r.Chance(0.25);
        hit2 += r.Chance(1u, 4u);
    }
    const double sigma = std::sqrt(kN * 0.25 * 0.75);
    CHECK(std::abs(static_cast<double>(hit) - kN * 0.25) < 6 * sigma);
    CHECK(std::abs(static_cast<double>(hit2) - kN * 0.25) < 6 * sigma);
}

void TestShuffle()
{
    Random r(8);
    // n = 4：24 种排列各自约 2000 次
    std::map<std::vector<int>, long> perms;
    for (int i = 0; i < 24 * 2000; ++i)
    {
        std::vector<int> v{0, 1, 2, 3};
        r.Shuffle(v);
        ++perms[v];
    }
    CHECK(perms.size() == 24);
    std::vector<long> counts;
    for (const auto& [p, c] : perms)
        counts.push_back(c);
    CHECK(ChiSquareOk(counts, 2000.0));

    // 结果始终是原序列的一个排列；空、单元素不崩
    std::vector<int> big(1000);
    for (int i = 0; i < 1000; ++i)
        big[static_cast<std::size_t>(i)] = i;
    auto shuffled = big;
    r.Shuffle(shuffled);
    CHECK(shuffled != big);
    std::sort(shuffled.begin(), shuffled.end());
    CHECK(shuffled == big);
    std::vector<int> empty, one{7};
    r.Shuffle(empty);
    r.Shuffle(one);
    CHECK(empty.empty() && one == std::vector<int>{7});

    std::deque<int> dq{1, 2, 3, 4, 5};
    r.Shuffle(dq);
    CHECK((std::set<int>(dq.begin(), dq.end()) == std::set<int>{1, 2, 3, 4, 5}));
    std::array<int, 5> arr{1, 2, 3, 4, 5};
    r.Shuffle(arr);
    CHECK((std::set<int>(arr.begin(), arr.end()) == std::set<int>{1, 2, 3, 4, 5}));
}

void TestElement()
{
    Random r(9);
    const std::vector<int> v{10, 20, 30, 40, 50};
    std::vector<long> bins(5, 0);
    for (int i = 0; i < 5 * 4000; ++i)
    {
        const int* p = r.Element(v);
        CHECK(p != nullptr && p >= v.data() && p < v.data() + v.size());
        ++bins[static_cast<std::size_t>(p - v.data())];
    }
    CHECK(ChiSquareOk(bins, 4000.0));

    std::vector<int> mut{1, 2, 3};
    int* mp = r.Element(mut);
    *mp = 99; // 非 const 容器给非 const 指针
    CHECK(std::count(mut.begin(), mut.end(), 99) == 1);

    const std::vector<int> empty;
    CHECK(r.Element(empty) == nullptr);
    const std::list<int> empty_list; // 空链表的 begin() 不是空指针，漏掉空检查会解引用哨兵节点
    CHECK(r.Element(empty_list) == nullptr);
    const std::array<int, 0> empty_array{};
    CHECK(r.Element(empty_array) == nullptr);

    const std::list<int> lst{4, 5, 6};
    std::set<int> seen;
    for (int i = 0; i < 1000; ++i)
        seen.insert(*r.Element(lst));
    CHECK((seen == std::set<int>{4, 5, 6}));

    const int arr[] = {1, 2, 3};
    CHECK(r.Element(arr) != nullptr);
}

void TestWeighted()
{
    Random r(21);
    CHECK(!r.Weighted(std::span<const std::uint32_t>{}).has_value());
    const std::vector<std::uint32_t> zeros{0, 0, 0};
    CHECK(!r.Weighted(zeros).has_value());

    // 零权重永远选不到；只有一个非零项时必选它
    const std::vector<std::uint32_t> single{0, 5, 0};
    for (int i = 0; i < 1000; ++i)
        CHECK(r.Weighted(single) == std::optional<std::size_t>(1));

    // 1:2:7
    const std::vector<std::uint32_t> w{1, 2, 7};
    std::vector<long> bins(3, 0);
    for (int i = 0; i < 100000; ++i)
        ++bins[*r.Weighted(w)];
    CHECK(ChiSquareOkWeighted(bins, {10000.0, 20000.0, 70000.0}));

    // 总和超过 2^32：用 32 位累加会回绕，选择就偏了
    const std::vector<std::uint32_t> huge{4000000000u, 4000000000u, 4000000000u};
    std::vector<long> hb(3, 0);
    for (int i = 0; i < 30000; ++i)
        ++hb[*r.Weighted(huge)];
    CHECK(ChiSquareOk(hb, 10000.0));
    const std::vector<std::uint32_t> maxw{0xffffffffu, 0xffffffffu};
    for (int i = 0; i < 1000; ++i)
        CHECK(r.Weighted(maxw).has_value());

    // 浮点权重：负数、NaN 当 0
    const std::vector<double> dw{std::nan(""), -1.0, 3.0, 0.0};
    for (int i = 0; i < 1000; ++i)
        CHECK(r.Weighted(dw) == std::optional<std::size_t>(2));
    const std::vector<double> none{std::nan(""), -1.0, 0.0};
    CHECK(!r.Weighted(none).has_value());
    const std::vector<double> inf{1.0, std::numeric_limits<double>::infinity()};
    CHECK(!r.Weighted(inf).has_value());
    // 累减的舍入误差：Uniform() 取到最大值 1-2^-53 时，pick 比逐项相减后的余量大一点点，扫完所有项还没落下，
    // 要落到最后一个有权重的项，不能返回"没有"。状态 {1, 0x4fc71c71c71c71c7, 3, 4} 的第一个输出恰好是 UINT64_MAX；
    // 权重是脚本搜出来的、会让累减走到末尾的一组。
    {
        const std::array<std::uint64_t, 4> max_state{1, 0x4fc71c71c71c71c7ULL, 3, 4};
        Random probe(max_state);
        CHECK(probe() == std::numeric_limits<std::uint64_t>::max());
        Random edge(max_state);
        const std::vector<double> ew{4.199999999999999, 44.0, 1.2};
        CHECK(edge.Weighted(ew) == std::optional<std::size_t>(2));
    }
    const std::vector<double> dr{1.0, 3.0};
    std::vector<long> db(2, 0);
    for (int i = 0; i < 40000; ++i)
        ++db[*r.Weighted(dr)];
    CHECK(ChiSquareOkWeighted(db, {10000.0, 30000.0}));
}

// 结果已定（概率 <= 0 或 >= 1、分母为 0、num >= den）时不消耗随机数：加一个 0% 的事件不会让后面所有的随机结果错位，
// 回放 / 复现时才稳。
void TestCertainChancesConsumeNothing()
{
    {
        Random a(77), b(77);
        CHECK(!a.Chance(0.0));
        CHECK(!a.Chance(-1.0));
        CHECK(!a.Chance(std::nan("")));
        CHECK(a.Chance(1.0));
        CHECK(a.Chance(2.0));
        CHECK(!a.Chance(3u, 0u));
        CHECK(a.Chance(5u, 5u));
        CHECK(a.Chance(6u, 5u));
        CHECK(a() == b());
    }
    {
        // min >= max 的区间、空权重同理
        Random a(78), b(78);
        CHECK(a.Range(4, 4) == 4);
        CHECK(a.Range(4, 3) == 4);
        CHECK(a.Range(2.0, 2.0) == 2.0);
        CHECK(!a.Weighted(std::span<const std::uint32_t>{}).has_value());
        CHECK(a() == b());
    }
}

void TestStateRoundTrip()
{
    Random a(7);
    for (int i = 0; i < 10; ++i)
        (void)a();
    const auto saved = a.State();
    Random b(saved);
    for (int i = 0; i < 100; ++i)
        CHECK(a() == b());

    Random c(7), d(8);
    CHECK(c() != d());

    // 全零状态不合法（会永远输出 0）：按种子 0 处理
    Random z(std::array<std::uint64_t, 4>{0, 0, 0, 0});
    Random zero_seed(0);
    for (int i = 0; i < 10; ++i)
        CHECK(z() == zero_seed());

    // 默认构造用 OS 熵：两个实例几乎必然不同
    Random e1, e2;
    CHECK(e1.State() != e2.State());
}

void TestStandardLibraryInterop()
{
    Random r(1);
    std::uniform_int_distribution<int> dist(0, 9);
    for (int i = 0; i < 1000; ++i)
    {
        const int v = dist(r);
        CHECK(v >= 0 && v <= 9);
    }
    std::vector<int> v{1, 2, 3, 4, 5, 6, 7, 8};
    std::shuffle(v.begin(), v.end(), ThreadLocalUrbg{});
    std::sort(v.begin(), v.end());
    CHECK((v == std::vector<int>{1, 2, 3, 4, 5, 6, 7, 8}));
}

void TestThreadLocalFunctions()
{
    for (int i = 0; i < 1000; ++i)
    {
        const int v = RandomInt(1, 6);
        CHECK(v >= 1 && v <= 6);
        const double d = RandomDouble(0.0, 1.0);
        CHECK(d >= 0.0 && d < 1.0);
    }
    CHECK(!RandomChance(0.0) && RandomChance(1.0));
    CHECK(!RandomChance(0u, 5u) && RandomChance(5u, 5u));
    std::vector<int> v{1, 2, 3};
    Shuffle(v);
    CHECK(RandomElement(v) != nullptr);
    CHECK(RandomWeighted(std::vector<std::uint32_t>{0, 3}) == std::optional<std::size_t>(1));

    // SetSeed 之后序列可复现，且就是 Random(seed) 的序列
    SetSeed(1234);
    Random ref(1234);
    for (int i = 0; i < 20; ++i)
        CHECK(ThreadRandom()() == ref());
    SetSeed(1234);
    CHECK(ThreadRandom()() == Random(1234)());
}

void TestThreadsAreSeededIndependently()
{
    constexpr int kThreads = 16;
    std::vector<std::uint64_t> first(kThreads, 0);
    std::vector<std::thread> ts;
    for (int i = 0; i < kThreads; ++i)
        ts.emplace_back([&first, i] { first[static_cast<std::size_t>(i)] = ThreadRandom()(); });
    for (auto& t : ts)
        t.join();
    CHECK(std::set<std::uint64_t>(first.begin(), first.end()).size() == kThreads);

    // 各线程 SetSeed 之后互不干扰、各自可复现
    std::vector<std::uint64_t> got(kThreads, 0);
    ts.clear();
    for (int i = 0; i < kThreads; ++i)
    {
        ts.emplace_back(
            [&got, i]
            {
                SetSeed(static_cast<std::uint64_t>(i));
                std::uint64_t acc = 0;
                for (int k = 0; k < 1000; ++k)
                    acc ^= ThreadRandom()();
                got[static_cast<std::size_t>(i)] = acc;
            });
    }
    for (auto& t : ts)
        t.join();
    for (int i = 0; i < kThreads; ++i)
    {
        Random ref(static_cast<std::uint64_t>(i));
        std::uint64_t acc = 0;
        for (int k = 0; k < 1000; ++k)
            acc ^= ref();
        CHECK(got[static_cast<std::size_t>(i)] == acc);
    }
}

// fork 之后父子进程不能吐出同一条序列：子进程的线程局部生成器要重新播种。
void TestForkReseeds()
{
    (void)ThreadRandom()(); // 让父进程已经播种
    int fds[2];
    CHECK(::pipe(fds) == 0);
    std::fflush(nullptr);
    const pid_t pid = ::fork();
    CHECK(pid >= 0);
    if (pid == 0)
    {
        ::close(fds[0]);
        const std::uint64_t v = ThreadRandom()();
        (void)!::write(fds[1], &v, sizeof(v));
        ::_exit(0);
    }
    ::close(fds[1]);
    std::uint64_t child = 0;
    CHECK(::read(fds[0], &child, sizeof(child)) == static_cast<ssize_t>(sizeof(child)));
    ::close(fds[0]);
    int status = 0;
    CHECK(::waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    const std::uint64_t parent = ThreadRandom()(); // 没重播种的话，这就是子进程吐出的那一个
    CHECK(parent != child);
}

} // namespace

int main()
{
    TestKnownAnswers();
    TestBelowKnownAnswers();
    TestRangeAndUniformKnownAnswers();
    TestBelowIsUnbiased();
    TestBelowUniform();
    TestRangeIntegers();
    TestFloating();
    TestChance();
    TestShuffle();
    TestElement();
    TestWeighted();
    TestCertainChancesConsumeNothing();
    TestStateRoundTrip();
    TestStandardLibraryInterop();
    TestThreadLocalFunctions();
    TestThreadsAreSeededIndependently();
    TestForkReseeds();
    std::puts("random_util_test: ok");
    return 0;
}
