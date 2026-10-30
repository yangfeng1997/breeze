// FlatHashMap / FlatHashSet（Swiss table）：
//   - 与 std::unordered_map / unordered_set 的随机对照（含 erase 密集、reserve / rehash / clear / 拷贝 / 移动 /
//   交换穿插），
//     每隔一段调 InvariantsHold() 自检控制字节镜像、满槽数、装载余量、每个元素可被查到；
//   - 极端哈希（全部同值、只有低 7 位不同、恒等哈希的 2
//   的幂倍数）、带状态的哈希器、对齐、移动语义、生命周期（不泄漏不重复析构）；
//   - 语义保证：键已存在的插入不扩容、墓碑不让容量失控、reserve 之后不扩容、异构查找不构造临时键；
//   - 被测的 Group 实现由 BREEZE_EXPECT_GROUP_WIDTH 指定（16 = SSE2，8 = 强制的
//   SWAR），同一份源码两个可执行文件各跑一遍。
// 无测试框架依赖：失败即 abort。

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "flat_hash_map.h"
#include "flat_hash_set.h"

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

#ifndef BREEZE_EXPECT_GROUP_WIDTH
#define BREEZE_EXPECT_GROUP_WIDTH 16
#endif

namespace
{

// ---------------- 测试用的键 / 哈希 ----------------

// 恒等哈希：std::hash<int> 本来就是它；2 的幂的倍数这类规律性的键最容易暴露"没有搅哈希"的问题。
struct IdentityHash
{
    std::size_t operator()(std::uint64_t v) const noexcept { return static_cast<std::size_t>(v); }
};

// 所有键同一个哈希值：表退化成线性探测，O(n^2)，但必须仍然正确。
struct ConstantHash
{
    std::size_t operator()(int) const noexcept { return 42; }
};

// 哈希值只有低 7 位有区别：H2 不同、H1 全同。
struct LowBitsHash
{
    std::size_t operator()(int v) const noexcept { return static_cast<std::size_t>(v) & 0x7f; }
};

// 带状态的哈希：默认构造的（seed = 0）与指定种子的结果不同。容器如果每次调用都默认构造一个哈希器，查找就会失败。
struct SeededHash
{
    std::uint64_t seed = 0;
    std::size_t operator()(int v) const noexcept
    {
        std::uint64_t x = static_cast<std::uint64_t>(v) + seed * 0x9e3779b97f4a7c15ULL;
        x ^= x >> 29;
        x *= 0xbf58476d1ce4e5b9ULL;
        x ^= x >> 32;
        return static_cast<std::size_t>(x);
    }
};

// ---------------- 对象生命周期计数 ----------------

struct Counters
{
    long live = 0, copies = 0, moves = 0;
};
Counters g_c;

struct Tracked
{
    int v;
    explicit Tracked(int x = 0)
        : v(x)
    {
        ++g_c.live;
    }
    Tracked(const Tracked& o)
        : v(o.v)
    {
        ++g_c.live;
        ++g_c.copies;
    }
    Tracked(Tracked&& o) noexcept
        : v(o.v)
    {
        ++g_c.live;
        ++g_c.moves;
    }
    Tracked& operator=(const Tracked& o)
    {
        v = o.v;
        ++g_c.copies;
        return *this;
    }
    Tracked& operator=(Tracked&& o) noexcept
    {
        v = o.v;
        ++g_c.moves;
        return *this;
    }
    ~Tracked() { --g_c.live; }
    bool operator==(const Tracked& o) const { return v == o.v; }
};
struct TrackedHash
{
    std::size_t operator()(const Tracked& t) const noexcept { return std::hash<int>{}(t.v); }
};

// ---------------- 通用对照 ----------------

std::string RandomString(std::mt19937_64& rng, std::size_t max_len)
{
    std::string s(rng() % (max_len + 1), 'a');
    for (char& c : s)
        c = static_cast<char>('a' + rng() % 4);
    return s;
}

// 把表的全部内容与参考实现逐项比对（大小、每个键都在且值相等、遍历恰好覆盖全部元素一次）
template <typename Map, typename Ref>
void CompareMap(const Map& m, const Ref& ref)
{
    CHECK(m.size() == ref.size());
    CHECK(m.empty() == ref.empty());
    std::size_t visited = 0;
    for (const auto& [k, v] : m)
    {
        const auto it = ref.find(k);
        CHECK(it != ref.end() && it->second == v);
        ++visited;
    }
    CHECK(visited == ref.size());
    for (const auto& [k, v] : ref)
    {
        const auto it = m.find(k);
        CHECK(it != m.end() && it->second == v);
        CHECK(m.contains(k) && m.count(k) == 1);
    }
    CHECK(m.InvariantsHold());
}

// 随机操作序列：每一步在被测表与 std::unordered_map 上做同一件事，比较返回值；周期性全量比对。
// MakeKey(rng) 从一个小的键空间里取键，保证碰撞、重复插入、删除不存在的键都频繁发生。
template <typename Map, typename Ref, typename MakeKey>
void RunMapDifferential(std::uint64_t seed, int steps, MakeKey make_key, Map map = Map())
{
    std::mt19937_64 rng(seed);
    Ref ref;
    for (int step = 0; step < steps; ++step)
    {
        const auto key = make_key(rng);
        const std::string val = RandomString(rng, 12);
        switch (rng() % 20)
        {
            case 0:
            case 1:
            {
                auto a = map.insert({key, val});
                auto b = ref.insert({key, val});
                CHECK(a.second == b.second && a.first->first == key && a.first->second == ref.at(key));
                break;
            }
            case 2:
            case 3:
            {
                auto a = map.try_emplace(key, val);
                auto b = ref.try_emplace(key, val);
                CHECK(a.second == b.second && a.first->second == b.first->second);
                break;
            }
            case 4:
            {
                auto a = map.emplace(key, val);
                auto b = ref.emplace(key, val);
                CHECK(a.second == b.second && a.first->second == b.first->second);
                break;
            }
            case 5:
            case 6:
            {
                map[key] = val;
                ref[key] = val;
                break;
            }
            case 7:
            {
                auto a = map.insert_or_assign(key, val);
                auto b = ref.insert_or_assign(key, val);
                CHECK(a.second == b.second && a.first->second == val);
                break;
            }
            case 8:
            case 9:
            case 10:
            case 11:
            {
                CHECK(map.erase(key) == ref.erase(key));
                break;
            }
            case 12:
            {
                const auto a = map.find(key);
                const auto b = ref.find(key);
                CHECK((a == map.end()) == (b == ref.end()));
                if (b != ref.end())
                    CHECK(a->second == b->second);
                break;
            }
            case 13:
            {
                if (!ref.empty())
                {
                    const auto k = ref.begin()->first; // 取一个确定存在的键
                    CHECK(map.at(k) == ref.at(k));
                    // 通过迭代器删除
                    auto it = map.find(k);
                    CHECK(it != map.end());
                    const std::size_t before = map.size();
                    map.erase(it);
                    ref.erase(k);
                    CHECK(map.size() == before - 1 && !map.contains(k));
                }
                break;
            }
            case 14:
                if (rng() % 8 == 0)
                    map.reserve(map.size() + rng() % 200);
                break;
            case 15:
                if (rng() % 8 == 0)
                    map.rehash(rng() % 400);
                break;
            case 16:
                if (rng() % 16 == 0)
                {
                    map.clear();
                    ref.clear();
                }
                break;
            case 17:
                if (rng() % 8 == 0)
                {
                    Map copy(map);
                    CHECK(copy == map);
                    map = std::move(copy); // 用拷贝替换自己（移动赋值）
                }
                break;
            case 18:
                if (rng() % 8 == 0)
                {
                    Map other;
                    other.swap(map);
                    CHECK(map.empty() && map.capacity() == 0 && map.InvariantsHold());
                    map.swap(other);
                }
                break;
            default:
                if (rng() % 32 == 0)
                    map.shrink_to_fit();
                break;
        }
        if (step % 97 == 0)
            CompareMap(map, ref);
    }
    CompareMap(map, ref);

    // 把剩下的元素一个个通过迭代器删光
    for (auto it = map.begin(); it != map.end();)
        it = map.erase(it);
    CHECK(map.empty() && map.InvariantsHold());
}

void TestDifferentialIntKeys()
{
    using M = FlatHashMap<int, std::string>;
    using R = std::unordered_map<int, std::string>;
    for (std::uint64_t seed = 1; seed <= 12; ++seed)
    {
        // 小键空间：大量重复与碰撞；大键空间：不断增长、触发多次扩容
        RunMapDifferential<M, R>(seed, 6000, [](std::mt19937_64& r) { return static_cast<int>(r() % 300); });
        RunMapDifferential<M, R>(seed + 100, 6000, [](std::mt19937_64& r) { return static_cast<int>(r() % 50000); });
    }
}

void TestDifferentialStringKeys()
{
    using M = FlatHashMap<std::string, std::string>;
    using R = std::unordered_map<std::string, std::string>;
    for (std::uint64_t seed = 1; seed <= 8; ++seed)
        RunMapDifferential<M, R>(seed, 5000, [](std::mt19937_64& r) { return RandomString(r, 40); }); // 跨过 SSO 边界
}

void TestDifferentialPatternedKeys()
{
    // 恒等哈希 + 规律性的键：2 的幂的倍数、高位不同低位全 0
    using M = FlatHashMap<std::uint64_t, std::string, IdentityHash>;
    using R = std::unordered_map<std::uint64_t, std::string>;
    for (std::uint64_t seed = 1; seed <= 6; ++seed)
    {
        RunMapDifferential<M, R>(seed, 5000, [](std::mt19937_64& r) { return (r() % 400) * 1024ULL; });
        RunMapDifferential<M, R>(seed + 50, 5000, [](std::mt19937_64& r) { return (r() % 400) << 40; });
        RunMapDifferential<M, R>(seed + 90, 5000, [](std::mt19937_64& r) { return (r() % 400) * 0x100000001ULL; });
    }
}

void TestDifferentialDegenerateHashes()
{
    using R = std::unordered_map<int, std::string>;
    for (std::uint64_t seed = 1; seed <= 4; ++seed)
    {
        // 全部同值：探测序列越来越长，规模要小
        RunMapDifferential<FlatHashMap<int, std::string, ConstantHash>, R>(
            seed, 1500, [](std::mt19937_64& r) { return static_cast<int>(r() % 200); });
        RunMapDifferential<FlatHashMap<int, std::string, LowBitsHash>, R>(
            seed, 3000, [](std::mt19937_64& r) { return static_cast<int>(r() % 2000); });
    }
}

// 带状态的哈希器：必须是容器里存着的那一个，而不是每次调用时默认构造的。
void TestStatefulHasher()
{
    using M = FlatHashMap<int, int, SeededHash>;
    M m(0, SeededHash{12345});
    for (int i = 0; i < 5000; ++i)
        m[i] = i;
    for (int i = 0; i < 5000; ++i)
        CHECK(m.at(i) == i);
    CHECK(m.hash_function().seed == 12345 && m.InvariantsHold());

    M copy(m);
    CHECK(copy.hash_function().seed == 12345 && copy == m);
    M moved(std::move(copy));
    CHECK(moved.hash_function().seed == 12345 && moved == m);
    M assigned;
    CHECK(assigned.hash_function().seed == 0);
    assigned = m;
    CHECK(assigned.hash_function().seed == 12345 && assigned == m);
    M other(0, SeededHash{777});
    other[1] = 1;
    other.swap(assigned);
    CHECK(other.hash_function().seed == 12345 && assigned.hash_function().seed == 777);
    CHECK(other == m && assigned.size() == 1 && assigned.at(1) == 1);
    assigned = std::move(other);
    CHECK(assigned.hash_function().seed == 12345 && assigned == m);

    // 重建（扩容、清墓碑）之后哈希器仍然是同一个
    for (int i = 0; i < 4000; ++i)
        m.erase(i);
    m.shrink_to_fit();
    for (int i = 4000; i < 5000; ++i)
        CHECK(m.at(i) == i);
    CHECK(m.InvariantsHold());
}

// ---------------- 集合 ----------------

void TestSetDifferential()
{
    for (std::uint64_t seed = 1; seed <= 10; ++seed)
    {
        std::mt19937_64 rng(seed);
        FlatHashSet<int> s;
        std::unordered_set<int> ref;
        const int space = seed % 2 == 0 ? 300 : 20000;
        for (int step = 0; step < 8000; ++step)
        {
            const int k = static_cast<int>(rng() % static_cast<std::uint64_t>(space));
            switch (rng() % 8)
            {
                case 0:
                case 1:
                case 2:
                    CHECK(s.insert(k).second == ref.insert(k).second);
                    break;
                case 3:
                    CHECK(s.emplace(k).second == ref.emplace(k).second);
                    break;
                case 4:
                case 5:
                    CHECK(s.erase(k) == ref.erase(k));
                    break;
                case 6:
                    CHECK(s.contains(k) == (ref.count(k) == 1) && s.count(k) == ref.count(k));
                    CHECK((s.find(k) != s.end()) == (ref.find(k) != ref.end()));
                    break;
                default:
                    if (rng() % 64 == 0)
                    {
                        s.clear();
                        ref.clear();
                    }
                    else if (rng() % 32 == 0)
                    {
                        s.rehash(rng() % 300);
                    }
                    break;
            }
            if (step % 111 == 0)
            {
                CHECK(s.size() == ref.size() && s.InvariantsHold());
                std::size_t n = 0;
                for (const int v : s)
                {
                    CHECK(ref.count(v) == 1);
                    ++n;
                }
                CHECK(n == ref.size());
            }
        }
    }
}

// 已存在的右值键不能被移走：insert 先查后构造，没有构造就没有移动。
// 集合的元素是 const 的：改了键就破坏哈希位置，所以连非 const 的迭代器也只给 const 引用。
static_assert(std::is_const_v<std::remove_reference_t<decltype(*std::declval<FlatHashSet<int>&>().begin())>>);
static_assert(std::is_const_v<std::remove_reference_t<decltype(*std::declval<const FlatHashSet<int>&>().begin())>>);
// map 的值可以改、键不可以
static_assert(
    !std::is_const_v<std::remove_reference_t<decltype(((*std::declval<FlatHashMap<int, int>&>().begin()).second))>>);
static_assert(
    std::is_const_v<std::remove_reference_t<decltype(((*std::declval<FlatHashMap<int, int>&>().begin()).first))>>);
static_assert(std::is_const_v<
              std::remove_reference_t<decltype(((*std::declval<const FlatHashMap<int, int>&>().begin()).second))>>);

void TestSetInsertDoesNotStealWhenPresent()
{
    struct PtrHash
    {
        std::size_t operator()(const std::unique_ptr<int>& p) const noexcept { return std::hash<int>{}(*p); }
    };
    struct PtrEq
    {
        bool operator()(const std::unique_ptr<int>& a, const std::unique_ptr<int>& b) const noexcept
        {
            return *a == *b;
        }
    };
    FlatHashSet<std::unique_ptr<int>, PtrHash, PtrEq> s;
    CHECK(s.insert(std::make_unique<int>(5)).second);
    auto dup = std::make_unique<int>(5);
    int* raw = dup.get();
    CHECK(!s.insert(std::move(dup)).second);
    CHECK(dup.get() == raw); // NOLINT：已存在，参数原封不动
    auto fresh = std::make_unique<int>(6);
    CHECK(s.insert(std::move(fresh)).second);
    CHECK(fresh == nullptr); // NOLINT：新元素被移进表里
    CHECK(s.size() == 2 && s.InvariantsHold());
}

// ---------------- 语义保证 ----------------

// 键已存在时的插入不能扩容 / 搬动元素（旧实现先 rehash 再查重，在装载边界上会白白让所有迭代器失效）
void TestExistingKeyInsertNeverRehashes()
{
    FlatHashMap<int, int> m;
    m.reserve(100);
    const std::size_t cap = m.capacity();
    for (int i = 0; m.size() < m.max_load(); ++i)
        m.try_emplace(i, i);
    CHECK(m.size() == m.max_load() && m.capacity() == cap);
    const int* addr = &m.find(0)->second;
    for (int round = 0; round < 3; ++round)
    {
        CHECK(!m.try_emplace(0, 99).second);
        CHECK(!m.insert({1, 99}).second);
        CHECK(!m.emplace(2, 99).second);
        CHECK(!m.insert_or_assign(3, 33).second);
        m[4] = 44;
        CHECK(m.capacity() == cap);        // 没有扩容
        CHECK(&m.find(0)->second == addr); // 没有搬动
    }
    CHECK(m.at(3) == 33 && m.at(4) == 44 && m.at(0) == 0);
    // 插入新键才会扩容
    CHECK(m.try_emplace(1000000, 1).second);
    CHECK(m.capacity() == cap * 2);
    CHECK(m.InvariantsHold());

    FlatHashSet<int> s;
    s.reserve(100);
    const std::size_t scap = s.capacity();
    for (int k = 0; s.size() < s.max_load(); ++k)
        s.insert(k);
    CHECK(!s.insert(0).second && s.capacity() == scap);
}

// reserve(n) 之后放 n 个以内不扩容、不搬动元素。
void TestReserveGuaranteesNoRehash()
{
    for (const std::size_t n : {1u, 7u, 14u, 15u, 100u, 1000u, 12345u})
    {
        FlatHashMap<std::uint64_t, int> m;
        m.reserve(n);
        const std::size_t cap = m.capacity();
        CHECK(m.max_load() >= n);
        for (std::size_t i = 0; i < n; ++i)
            m.try_emplace(i * 2654435761ULL, static_cast<int>(i));
        CHECK(m.capacity() == cap && m.size() == n && m.InvariantsHold());
    }
    FlatHashMap<int, int> m;
    m.reserve(0);
    CHECK(m.capacity() == 0);
}

// 已经有足够余量时 reserve 是空操作：不重新分配、不搬动元素。
void TestReserveWhenRoomIsAlreadyThere()
{
    FlatHashMap<int, int> m;
    m.reserve(100);
    for (int i = 0; i < 10; ++i)
        m[i] = i;
    const int* addr = &m.find(0)->second;
    const std::size_t cap = m.capacity();
    m.reserve(50);
    m.reserve(m.max_load());
    m.reserve(0);
    CHECK(m.capacity() == cap && &m.find(0)->second == addr);
}

// 表处在装载上限、刚删掉一个又把同一个键插回去：它会落回刚空出来的位置，不需要装载余量，不能因此扩容或重建。
// （位置是墓碑时 growth_left 仍是 0，如果"放进墓碑也算消耗余量"就会白白翻倍。）
void TestReinsertAfterEraseAtFullLoadNeverGrows()
{
    FlatHashMap<std::uint64_t, std::uint64_t> m;
    m.reserve(1000);
    std::mt19937_64 rng(3);
    std::vector<std::uint64_t> keys;
    while (m.size() < m.max_load())
    {
        keys.push_back(rng());
        m[keys.back()] = keys.size();
    }
    const std::size_t cap = m.capacity();
    const auto* probe = &m.find(keys[0])->second;
    int tombstone_cases = 0;
    for (std::size_t i = 0; i < keys.size(); i += 3)
    {
        const std::uint64_t k = keys[i];
        const std::uint64_t v = m.at(k);
        CHECK(m.erase(k) == 1);
        CHECK(m.try_emplace(k, v).second);
        CHECK(m.capacity() == cap);
        CHECK(&m.find(keys[0])->second == probe); // 没有搬动元素（被删又插回去的也落回原位）
        ++tombstone_cases;
    }
    CHECK(tombstone_cases > 100 && m.size() == m.max_load() && m.InvariantsHold());
}

// 稀疏的表里删除元素不留墓碑：前后窗口都有空位且连续满槽很短，没有探测序列越过过它，直接置空（并归还装载余量）。
// 留了墓碑的话，墓碑会一直占着探测序列和装载余量，直到下一次重建。
void TestEraseInSparseTableLeavesNoTombstones()
{
    FlatHashMap<std::uint64_t, int> m;
    m.reserve(500); // 容量 1024 左右，放 40 个：装载 < 4%
    std::mt19937_64 rng(9);
    std::vector<std::uint64_t> keys;
    for (int i = 0; i < 40; ++i)
    {
        keys.push_back(rng());
        m[keys.back()] = i;
    }
    for (const auto k : keys)
        CHECK(m.erase(k) == 1);
    CHECK(m.tombstones() == 0 && m.empty() && m.InvariantsHold());
    // 反复删插同一批键，墓碑也不累积
    for (int round = 0; round < 50; ++round)
    {
        for (const auto k : keys)
            m[k] = round;
        for (const auto k : keys)
            m.erase(k);
    }
    CHECK(m.tombstones() == 0 && m.InvariantsHold());

    // 中等装载（约 50%）：绝大多数删除仍然不留墓碑（实测：16 宽的 SSE 组 0 个，8 宽的 SWAR 组约 17%）。
    // 删除优化的判断要是偏保守（比如前后窗口里连续满槽数算大了），这里的比例会迅速涨到 50% 左右。
    {
        FlatHashMap<std::uint64_t, int> half;
        half.reserve(1500);
        std::vector<std::uint64_t> ks;
        const std::size_t target = half.capacity() / 2;
        while (half.size() < target)
        {
            ks.push_back(rng());
            half[ks.back()] = 1;
        }
        std::size_t erased = 0;
        for (std::size_t i = 0; i < ks.size() && erased < ks.size() / 2; ++i, ++erased)
            half.erase(ks[i]);
        CHECK(half.tombstones() * 4 <= erased);
        CHECK(half.InvariantsHold());
    }

    // 高装载时才会留墓碑，且重建（rehash / 扩容）之后清零
    FlatHashMap<std::uint64_t, int> dense;
    for (int i = 0; i < 2000; ++i)
        dense[rng()] = i;
    std::vector<std::uint64_t> some;
    for (const auto& [k, v] : dense)
    {
        if (some.size() < 300)
            some.push_back(k);
    }
    for (const auto k : some)
        dense.erase(k);
    dense.rehash(dense.capacity());
    CHECK(dense.tombstones() == 0 && dense.InvariantsHold());
}

// insert(value_type&&)：键是 const 的只能拷贝，值要移走（旧实现的 const_cast 是未定义行为，这里是 try_emplace(v.first,
// std::move(v.second))）。
void TestInsertRvaluePairMovesTheValue()
{
    g_c = {};
    {
        FlatHashMap<int, Tracked> m;
        using Pair = std::pair<const int, Tracked>;
        m.insert(Pair(1, Tracked(5)));
        CHECK(g_c.copies == 0 && m.at(1).v == 5); // 值是移进去的
        Pair dup(1, Tracked(6));
        CHECK(!m.insert(std::move(dup)).second);
        CHECK(dup.second.v == 6 && m.at(1).v == 5); // NOLINT：键已存在，值没被移走、也没被覆盖
    }
    CHECK(g_c.live == 0);
}

void TestCapacityBehavior()
{
    FlatHashMap<int, int> m;
    CHECK(m.capacity() == 0 && m.size() == 0 && m.begin() == m.end() && m.InvariantsHold());
    CHECK(!m.contains(1) && m.find(1) == m.end() && m.erase(1) == 0);
    m.clear(); // 空表 clear
    for (int i = 0; i < 100000; ++i)
    {
        m[i] = i;
        const std::size_t cap = m.capacity();
        CHECK(cap >= container_internal::kGroupWidth && (cap & (cap - 1)) == 0); // 2 的幂
        CHECK(m.size() <= m.max_load());
    }
    CHECK(m.capacity() <= 262144); // 10 万个元素在 7/8 装载下不会超过 2^18
    CHECK(m.load_factor() > 0.3 && m.load_factor() <= 0.875);
    m.clear();
    CHECK(m.size() == 0 && m.capacity() > 0 && m.InvariantsHold()); // clear 保留容量
    CHECK(m.begin() == m.end());
    m.shrink_to_fit();
    CHECK(m.capacity() == 0); // 空表收缩到不占存储
    m[1] = 1;
    CHECK(m.at(1) == 1);
}

// 反复"删一个、插一个"让表长期处在高装载：墓碑要被清理，容量不能无限涨，也不能每次插入都重建（旧实现的最坏情况）。
void TestTombstoneChurn()
{
    FlatHashMap<std::uint64_t, std::uint64_t> m;
    std::mt19937_64 rng(7);
    constexpr std::uint64_t kLive = 12000; // 稳态元素数：落在装载 7/8 附近
    std::vector<std::uint64_t> keys;
    for (std::uint64_t i = 0; i < kLive; ++i)
    {
        keys.push_back(rng());
        m[keys.back()] = i;
    }
    const std::size_t steady_cap = m.capacity();
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 1500000; ++i)
    {
        const std::size_t pos = rng() % keys.size();
        CHECK(m.erase(keys[pos]) == 1);
        keys[pos] = rng();
        m[keys[pos]] = static_cast<std::uint64_t>(i);
    }
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(m.size() == kLive);
    CHECK(m.capacity() <= steady_cap * 2); // 容量没有失控
    CHECK(m.InvariantsHold());
    CHECK(ms < 20000); // 没有陷入"每次插入都重建"的二次方行为（Release 下一般 < 1s，消毁器下放宽）
    for (const auto k : keys)
        CHECK(m.contains(k));
}

// ---------------- 迭代 ----------------

void TestIteration()
{
    FlatHashMap<int, int> m;
    for (int i = 0; i < 1000; ++i)
        m[i] = i;
    std::vector<int> seen;
    for (auto it = m.begin(); it != m.end(); ++it)
        seen.push_back(it->first);
    std::sort(seen.begin(), seen.end());
    for (int i = 0; i < 1000; ++i)
        CHECK(seen[static_cast<std::size_t>(i)] == i); // 每个元素恰好一次

    // 后置自增、iterator -> const_iterator、const 表的遍历
    auto it = m.begin();
    auto old = it++;
    CHECK(old != it);
    FlatHashMap<int, int>::const_iterator cit = m.begin();
    CHECK(cit == m.cbegin() && cit != m.cend());
    const auto& cm = m;
    std::size_t n = 0;
    for (const auto& kv : cm)
    {
        (void)kv;
        ++n;
    }
    CHECK(n == 1000);

    // 通过迭代器改值，键是 const 的
    for (auto& [k, v] : m)
        v = k * 2;
    for (int i = 0; i < 1000; ++i)
        CHECK(m.at(i) == i * 2);
    static_assert(std::is_same_v<std::remove_reference_t<decltype((*m.begin()).first)>, const int>);

    // 很稀疏的大表：遍历按组跳过空槽，找得到那几个元素
    FlatHashMap<int, int> sparse;
    sparse.reserve(1000000);
    for (const int k : {1, 77777, 500000, 999999})
        sparse[k] = k;
    std::set<int> got;
    for (const auto& [k, v] : sparse)
        got.insert(k);
    CHECK((got == std::set<int>{1, 77777, 500000, 999999}));

    // 边界：容量刚好填满到只剩最后一个槽附近、墓碑夹在中间时遍历不能漏也不能重
    FlatHashMap<int, int> t;
    for (int i = 0; i < 5000; ++i)
        t[i] = i;
    for (int i = 0; i < 5000; i += 3)
        t.erase(i);
    std::size_t cnt = 0;
    for (const auto& kv : t)
    {
        CHECK(kv.first % 3 != 0);
        ++cnt;
    }
    CHECK(cnt == t.size());
}

void TestEraseWhileIterating()
{
    FlatHashMap<int, int> m;
    for (int i = 0; i < 10000; ++i)
        m[i] = i;
    // erase(iterator) 返回下一个，边遍历边删
    for (auto it = m.begin(); it != m.end();)
    {
        if (it->first % 2 == 0)
            it = m.erase(it);
        else
            ++it;
    }
    CHECK(m.size() == 5000 && m.InvariantsHold());
    for (int i = 0; i < 10000; ++i)
        CHECK(m.contains(i) == (i % 2 == 1));

    CHECK(erase_if(m, [](const auto& kv) { return kv.first % 3 == 0; }) == 1667); // 奇数中 3 的倍数：3,9,...,9999
    for (const auto& kv : m)
        CHECK(kv.first % 2 == 1 && kv.first % 3 != 0);

    // 区间删除
    FlatHashMap<int, int> r;
    for (int i = 0; i < 100; ++i)
        r[i] = i;
    const auto next = r.erase(r.begin(), r.end());
    CHECK(next == r.end() && r.empty() && r.InvariantsHold());

    FlatHashSet<int> s;
    for (int i = 0; i < 1000; ++i)
        s.insert(i);
    CHECK(erase_if(s, [](int v) { return v >= 500; }) == 500 && s.size() == 500);
}

// ---------------- 拷贝 / 移动 / 交换 ----------------

void TestCopyMoveSwap()
{
    FlatHashMap<std::string, std::string> a;
    for (int i = 0; i < 2000; ++i)
        a["key" + std::to_string(i)] = "value" + std::to_string(i);
    for (int i = 0; i < 2000; i += 2)
        a.erase("key" + std::to_string(i)); // 留下墓碑
    CHECK(a.InvariantsHold());

    // 拷贝：互相独立
    FlatHashMap<std::string, std::string> b(a);
    CHECK(a == b && b.InvariantsHold());
    b["key1"] = "changed";
    b["extra"] = "x";
    CHECK(a.at("key1") == "value1" && !a.contains("extra"));
    CHECK(!(a == b));

    // 拷贝赋值（含自赋值）、移动
    FlatHashMap<std::string, std::string> c;
    c = a;
    CHECK(c == a);
    auto& c_ref = c;
    c = c_ref; // 自赋值
    CHECK(c == a);
    FlatHashMap<std::string, std::string> d(std::move(c));
    CHECK(d == a);
    // 被移走的表：有效、为空、可以继续使用（旧的 LinkedHashMap 在这里会留下悬空的链表头）
    CHECK(c.empty() && c.size() == 0 && c.capacity() == 0 && c.begin() == c.end() && c.InvariantsHold()); // NOLINT
    c["again"] = "yes";                                                                                   // NOLINT
    CHECK(c.size() == 1 && c.at("again") == "yes" && c.InvariantsHold());                                 // NOLINT
    auto& d_ref = d;
    d = std::move(d_ref); // 自移动赋值：不能把自己清掉
    CHECK(d == a);

    // 移动赋值覆盖一个非空的表
    FlatHashMap<std::string, std::string> e;
    e["old"] = "gone";
    e = std::move(d);
    CHECK(e == a && !e.contains("old") && d.empty()); // NOLINT

    // 交换：容量不同的两个表
    FlatHashMap<std::string, std::string> small;
    small["s"] = "1";
    small.swap(e);
    CHECK(e.size() == 1 && e.at("s") == "1" && small == a);
    swap(small, e);
    CHECK(small.size() == 1 && e == a);

    // 初始化列表、迭代器区间构造、列表赋值
    FlatHashMap<int, int> il{{1, 10}, {2, 20}, {3, 30}, {2, 99}}; // 重复的键保留第一个
    CHECK(il.size() == 3 && il.at(2) == 20);
    std::vector<std::pair<int, int>> v{{5, 50}, {6, 60}};
    FlatHashMap<int, int> from_range(v.begin(), v.end());
    CHECK(from_range.size() == 2 && from_range.at(5) == 50);
    il = {{7, 70}};
    CHECK(il.size() == 1 && il.at(7) == 70 && !il.contains(1));

    // operator==：与容量、插入顺序无关
    FlatHashMap<int, int> p, q;
    for (int i = 0; i < 100; ++i)
        p[i] = i;
    for (int i = 99; i >= 0; --i)
        q[i] = i;
    q.reserve(10000);
    CHECK(p == q);
    q[50] = -1;
    CHECK(p != q);
    q[50] = 50;
    q[1000] = 1;
    CHECK(p != q);

    FlatHashSet<int> s1{1, 2, 3}, s2{3, 2, 1}, s3{1, 2};
    CHECK(s1 == s2 && s1 != s3);
    FlatHashSet<int> s4(s1);
    FlatHashSet<int> s5(std::move(s4));
    CHECK(s5 == s1 && s4.empty()); // NOLINT
}

// ---------------- 类型 ----------------

void TestMoveOnlyValues()
{
    FlatHashMap<int, std::unique_ptr<int>> m;
    for (int i = 0; i < 3000; ++i)
        m.try_emplace(i, std::make_unique<int>(i * 3));
    for (int i = 0; i < 3000; i += 2)
        m.erase(i);
    for (int i = 0; i < 3000; ++i)
    {
        const auto it = m.find(i);
        if (i % 2 == 0)
        {
            CHECK(it == m.end());
        }
        else
        {
            CHECK(it != m.end() && *it->second == i * 3);
        }
    }
    // try_emplace 对已存在的键不消耗参数
    auto p = std::make_unique<int>(1);
    CHECK(!m.try_emplace(1, std::move(p)).second);
    CHECK(p != nullptr); // NOLINT
    // 改用 insert_or_assign 才会覆盖
    m.insert_or_assign(1, std::make_unique<int>(-1));
    CHECK(*m.at(1) == -1);
    auto moved = std::move(m);
    CHECK(moved.size() == 1500 && m.empty()); // NOLINT
}

struct alignas(64) Aligned64
{
    std::uint64_t v = 0;
};
struct alignas(32) Aligned32Key
{
    std::uint64_t v = 0;
    bool operator==(const Aligned32Key& o) const { return v == o.v; }
};
struct Aligned32Hash
{
    std::size_t operator()(const Aligned32Key& k) const noexcept { return std::hash<std::uint64_t>{}(k.v); }
};

// 槽的对齐超过默认的 new 对齐：每个元素的地址都要满足它自己的对齐（旧实现用 new uint8_t[]，会错位）。
void TestOverAlignedSlots()
{
    FlatHashMap<int, Aligned64> m;
    for (int i = 0; i < 2000; ++i)
        m[i].v = static_cast<std::uint64_t>(i);
    for (int i = 0; i < 2000; i += 3)
        m.erase(i);
    for (auto& [k, v] : m)
    {
        CHECK(reinterpret_cast<std::uintptr_t>(&v) % 64 == 0);
        CHECK(v.v == static_cast<std::uint64_t>(k));
    }
    FlatHashSet<Aligned32Key, Aligned32Hash> s;
    for (std::uint64_t i = 0; i < 1000; ++i)
        s.insert(Aligned32Key{i});
    for (const auto& k : s)
        CHECK(reinterpret_cast<std::uintptr_t>(&k) % 32 == 0);
    CHECK(s.size() == 1000 && s.InvariantsHold() && m.InvariantsHold());
}

// 生命周期：构造与析构配平（没有泄漏、没有重复析构），扩容时是移动不是拷贝。
void TestLifetimeAccounting()
{
    g_c = {};
    {
        FlatHashMap<Tracked, Tracked, TrackedHash> m;
        for (int i = 0; i < 3000; ++i)
            m.try_emplace(Tracked(i), Tracked(i));
        CHECK(g_c.copies == 0); // 扩容（多次）与插入都只移动
        for (int i = 0; i < 3000; i += 2)
            m.erase(Tracked(i));
        m.rehash(100000);
        m.shrink_to_fit();
        CHECK(g_c.copies == 0);
        CHECK(g_c.live == 2 * 1500 /* 表里的 */);
        FlatHashMap<Tracked, Tracked, TrackedHash> copy(m);
        CHECK(g_c.live == 2 * 3000);
        CHECK(g_c.copies == 2 * 1500); // 拷贝构造：每个键值各一次
        m.clear();
        CHECK(g_c.live == 2 * 1500);
        m = copy; // 拷贝赋值
        CHECK(g_c.live == 2 * 3000);
    }
    CHECK(g_c.live == 0);

    g_c = {};
    {
        FlatHashSet<Tracked, TrackedHash> s;
        for (int i = 0; i < 2000; ++i)
            s.emplace(i);
        CHECK(g_c.copies == 0);
        for (int i = 0; i < 2000; i += 2)
            s.erase(Tracked(i));
        FlatHashSet<Tracked, TrackedHash> s2(std::move(s));
        CHECK(g_c.live == 1000);
    }
    CHECK(g_c.live == 0);
}

// 常见的写法都能编译、行为符合直觉（字符串字面量、make_pair、piecewise、初始化列表、结构化绑定、const 表）。
template <typename M>
void ExerciseCommonApiForms()
{
    M m{{"a", 1}, {"b", 2}};
    CHECK(m.size() == 2);
    m.insert(std::make_pair(std::string("c"), 3));
    m.insert(std::pair<const std::string, int>("d", 4));
    m.emplace(std::piecewise_construct, std::forward_as_tuple("e"), std::forward_as_tuple(5));
    m.emplace("f", 6);
    m.try_emplace("g", 7);
    m.insert_or_assign("g", 70);
    std::string key = "h";
    m.try_emplace(key, 8);
    CHECK(!m.try_emplace(std::move(key), 9).second);
    m["i"] = 10;
    CHECK(m.at("g") == 70 && m.at("h") == 8 && m.size() == 9);
    for (auto& [k, v] : m)
    {
        (void)k;
        v += 1;
    }
    const M& cm = m;
    int sum = 0;
    for (const auto& [k, v] : cm)
    {
        (void)k;
        sum += v;
    }
    CHECK(sum == 2 + 3 + 4 + 5 + 6 + 7 + 71 + 9 + 11);
    const auto it = m.find("a");
    CHECK(it != m.end() && it->second == 2);
    M copy = m;
    CHECK(copy == m);
    m.erase("a");
    CHECK(!m.contains("a") && copy.contains("a"));
    CHECK(erase_if(m, [](const auto& kv) { return kv.second > 8; }) == 3);
}

template <typename S>
void ExerciseCommonSetForms()
{
    S s{"x", "y"};
    s.insert(std::string("z"));
    s.emplace("w");
    for (const auto& k : s)
        CHECK(!k.empty());
    CHECK(s.contains("x") && s.size() == 4 && s.erase("x") == 1);
    S c = s;
    CHECK(c == s);
}

void TestCommonApiForms()
{
    ExerciseCommonApiForms<FlatHashMap<std::string, int>>();
    ExerciseCommonSetForms<FlatHashSet<std::string>>();
}

// ---------------- 构造抛异常 ----------------

// 拷贝时抛（只抛 boom 的那个）。移动不抛（容器要求）。
struct Thrower
{
    int v = 0;
    bool boom = false;
    Thrower() = default;
    Thrower(int x, bool b)
        : v(x)
        , boom(b)
    {
    }
    Thrower(const Thrower& o)
        : v(o.v)
        , boom(o.boom)
    {
        if (boom)
            throw std::runtime_error("copy");
    }
    Thrower(Thrower&& o) noexcept
        : v(o.v)
        , boom(false)
    {
    }
    Thrower& operator=(const Thrower&) = default;
    Thrower& operator=(Thrower&&) noexcept = default;
};

// 元素构造抛异常：表里什么也没登记（先构造、后改控制字节），表仍然有效、大小不变、可以继续用；拷贝整个表中途抛，不泄漏。
void TestConstructionThrowing()
{
    FlatHashMap<int, Thrower> m;
    for (int i = 0; i < 100; ++i)
        m.try_emplace(i, Thrower(i, false));
    const Thrower bad(1000, true);

    bool threw = false;
    try
    {
        m.try_emplace(1000, bad); // 值拷贝构造时抛
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }
    CHECK(threw && m.size() == 100 && !m.contains(1000) && m.InvariantsHold());

    // 同样在刚好需要扩容的那一次插入上抛：扩容已经完成，但新元素没有登记
    FlatHashMap<int, Thrower> full;
    full.reserve(14);
    for (int i = 0; full.size() < full.max_load(); ++i)
        full.try_emplace(i, Thrower(i, false));
    const std::size_t full_size = full.size();
    threw = false;
    try
    {
        full.try_emplace(999, bad);
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }
    CHECK(threw && full.size() == full_size && !full.contains(999) && full.InvariantsHold());
    full.try_emplace(998, Thrower(998, false));
    CHECK(full.contains(998) && full.InvariantsHold());

    // 拷贝整个表时有一个元素的拷贝会抛：异常传出来，已经拷好的被销毁（ASAN 下查泄漏），原表不受影响
    FlatHashMap<int, Thrower> with_bad;
    for (int i = 0; i < 50; ++i)
        with_bad.try_emplace(i, Thrower(i, false));
    with_bad.at(25).boom = true;
    threw = false;
    try
    {
        FlatHashMap<int, Thrower> copy(with_bad);
        (void)copy;
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }
    CHECK(threw && with_bad.size() == 50 && with_bad.InvariantsHold());
    with_bad.at(25).boom = false;
    FlatHashMap<int, Thrower> ok(with_bad);
    CHECK(ok.size() == 50 && ok.InvariantsHold());
}

// ---------------- 异构查找 ----------------

struct Key
{
    std::string s;
    bool operator==(const Key& o) const { return s == o.s; }
};
long g_key_copies = 0;
struct CountingKey
{
    std::string s;
    explicit CountingKey(std::string x)
        : s(std::move(x))
    {
    }
    CountingKey(const CountingKey& o)
        : s(o.s)
    {
        ++g_key_copies;
    }
    CountingKey(CountingKey&& o) noexcept
        : s(std::move(o.s))
    {
    }
};
struct KeyView
{
    std::string_view s;
};
struct TransparentHash
{
    using is_transparent = void;
    std::size_t operator()(const CountingKey& k) const noexcept { return std::hash<std::string_view>{}(k.s); }
    std::size_t operator()(const KeyView& k) const noexcept { return std::hash<std::string_view>{}(k.s); }
};
struct TransparentEq
{
    using is_transparent = void;
    bool operator()(const CountingKey& a, const CountingKey& b) const noexcept { return a.s == b.s; }
    bool operator()(const CountingKey& a, const KeyView& b) const noexcept { return a.s == b.s; }
    bool operator()(const KeyView& a, const CountingKey& b) const noexcept { return a.s == b.s; }
};

struct StringHash
{
    using is_transparent = void;
    std::size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
};

void TestHeterogeneousLookup()
{
    FlatHashMap<CountingKey, int, TransparentHash, TransparentEq> m;
    for (int i = 0; i < 500; ++i)
        m.try_emplace(CountingKey("k" + std::to_string(i)), i);
    g_key_copies = 0;
    // 用 KeyView 查：不构造临时的 CountingKey（构造它要拷贝 string；这里用拷贝计数证明没有发生）
    for (int i = 0; i < 500; ++i)
    {
        const std::string name = "k" + std::to_string(i);
        const KeyView kv{name};
        CHECK(m.contains(kv) && m.count(kv) == 1);
        const auto it = m.find(kv);
        CHECK(it != m.end() && it->second == i);
        CHECK(m.at(kv) == i);
    }
    CHECK(!m.contains(KeyView{"nope"}));
    CHECK(m.erase(KeyView{"k7"}) == 1 && m.erase(KeyView{"k7"}) == 0);
    CHECK(g_key_copies == 0);

    // 常见用法：string 键的表用 string_view 查
    FlatHashMap<std::string, int, StringHash, std::equal_to<>> names;
    names["alice"] = 1;
    names["bob"] = 2;
    const char* raw = "alice";
    CHECK(names.contains(raw) && names.at(std::string_view("bob")) == 2 &&
          names.find(std::string_view("zed")) == names.end());
    FlatHashSet<std::string, StringHash, std::equal_to<>> set;
    set.insert("x");
    CHECK(set.contains(std::string_view("x")) && set.erase(std::string_view("x")) == 1);

    // 普通（非透明）的表：查找参数是 key_type，别的类型靠隐式转换
    FlatHashMap<std::string, int> plain;
    plain["abc"] = 1;
    CHECK(plain.contains("abc") && plain.at("abc") == 1 && plain.erase("abc") == 1);
}

// at() 找不到键：abort（库不抛异常）。
void TestAtMissingAborts()
{
    std::fflush(nullptr);
    const pid_t pid = ::fork();
    CHECK(pid >= 0);
    if (pid == 0)
    {
        std::freopen("/dev/null", "w", stderr);
        FlatHashMap<int, int> m;
        m[1] = 1;
        (void)m.at(2);
        ::_exit(0); // 不应该走到这里
    }
    int status = 0;
    CHECK(::waitpid(pid, &status, 0) == pid);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);

    FlatHashMap<int, int> ok;
    ok[1] = 11;
    const auto& cok = ok;
    CHECK(ok.at(1) == 11 && cok.at(1) == 11);
}

// 大规模：几十万个元素的插入、命中、未命中、删除，校验和对得上。
void TestLargeScale()
{
    constexpr std::uint64_t kN = 300000;
    FlatHashMap<std::uint64_t, std::uint64_t> m;
    std::unordered_map<std::uint64_t, std::uint64_t> ref;
    std::mt19937_64 rng(2024);
    for (std::uint64_t i = 0; i < kN; ++i)
    {
        const std::uint64_t k = rng();
        m[k] = i;
        ref[k] = i;
    }
    CHECK(m.size() == ref.size() && m.InvariantsHold());
    std::uint64_t sum = 0, ref_sum = 0;
    for (const auto& [k, v] : m)
        sum += k ^ v;
    for (const auto& [k, v] : ref)
        ref_sum += k ^ v;
    CHECK(sum == ref_sum);
    for (std::uint64_t i = 0; i < 100000; ++i)
    {
        const std::uint64_t k = rng(); // 几乎必然不存在
        CHECK(m.contains(k) == (ref.count(k) == 1));
    }
    std::size_t erased = 0;
    for (const auto& [k, v] : ref)
    {
        if (v % 3 == 0)
        {
            CHECK(m.erase(k) == 1);
            ++erased;
        }
    }
    CHECK(m.size() == ref.size() - erased && m.InvariantsHold());
}

} // namespace

int main()
{
    CHECK(container_internal::kGroupWidth == BREEZE_EXPECT_GROUP_WIDTH);
    TestDifferentialIntKeys();
    TestDifferentialStringKeys();
    TestDifferentialPatternedKeys();
    TestDifferentialDegenerateHashes();
    TestStatefulHasher();
    TestSetDifferential();
    TestSetInsertDoesNotStealWhenPresent();
    TestExistingKeyInsertNeverRehashes();
    TestReserveGuaranteesNoRehash();
    TestReserveWhenRoomIsAlreadyThere();
    TestEraseInSparseTableLeavesNoTombstones();
    TestInsertRvaluePairMovesTheValue();
    TestReinsertAfterEraseAtFullLoadNeverGrows();
    TestCapacityBehavior();
    TestTombstoneChurn();
    TestIteration();
    TestEraseWhileIterating();
    TestCopyMoveSwap();
    TestMoveOnlyValues();
    TestOverAlignedSlots();
    TestLifetimeAccounting();
    TestCommonApiForms();
    TestConstructionThrowing();
    TestHeterogeneousLookup();
    TestAtMissingAborts();
    TestLargeScale();
    std::puts("flat_hash_test: ok");
    return 0;
}
