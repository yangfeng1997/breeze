// LinkedHashMap / LinkedHashSet（保持插入顺序的哈希表）：
//   - 与"std::list + unordered_map"的经典实现随机对照：每一步比较结果，周期性地正向、反向比对整个顺序，并调
//   InvariantsHold()
//     自检两条链（顺序链、冲突链）、数组紧凑、哈希值、每个元素可被查到；
//   - swap-and-pop 带来的几个边角：erase 返回的"下一个"恰好是被搬动的最后一个元素、区间删除时区间末端被搬动、删光再插；
//   - 被移走的表仍然是完整的"空表"（旧实现会留下悬空的链头尾，begin() != end() 且再插入就越界）；
//   - LRU 用法（move_to_back + pop_front）与独立实现的 LRU 对照；
//   - 极端哈希、带状态的哈希器、对齐、生命周期、异构查找、at() 找不到就 abort。
// 无测试框架依赖：失败即 abort。

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <list>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "base/container/linked_hash_map.h"
#include "base/container/linked_hash_set.h"

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

struct ConstantHash
{
    std::size_t operator()(int) const noexcept { return 7; }
};
struct IdentityHash
{
    std::size_t operator()(std::uint64_t v) const noexcept { return static_cast<std::size_t>(v); }
};
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

// ---------------- 参考实现：list 维护顺序，unordered_map 记位置 ----------------

template <typename K, typename V>
struct Ref
{
    using List = std::list<std::pair<K, V>>;
    List order;
    std::unordered_map<K, typename List::iterator> index;

    bool Insert(const K& k, const V& v)
    {
        if (index.count(k))
            return false;
        order.emplace_back(k, v);
        index[k] = std::prev(order.end());
        return true;
    }
    bool Erase(const K& k)
    {
        auto it = index.find(k);
        if (it == index.end())
            return false;
        order.erase(it->second);
        index.erase(it);
        return true;
    }
    void MoveToBack(const K& k) { order.splice(order.end(), order, index.at(k)); }
    void MoveToFront(const K& k) { order.splice(order.begin(), order, index.at(k)); }
    void Clear()
    {
        order.clear();
        index.clear();
    }
};

template <typename Map, typename R>
void CompareOrdered(const Map& m, const R& ref)
{
    CHECK(m.size() == ref.order.size());
    CHECK(m.empty() == ref.order.empty());
    // 正向
    auto it = m.begin();
    for (const auto& [k, v] : ref.order)
    {
        CHECK(it != m.end());
        CHECK(it->first == k && it->second == v);
        ++it;
    }
    CHECK(it == m.end());
    // 反向
    auto rit = m.rbegin();
    for (auto r = ref.order.rbegin(); r != ref.order.rend(); ++r)
    {
        CHECK(rit != m.rend());
        CHECK(rit->first == r->first && rit->second == r->second);
        ++rit;
    }
    CHECK(rit == m.rend());
    if (!ref.order.empty())
    {
        CHECK(m.front().first == ref.order.front().first && m.back().first == ref.order.back().first);
    }
    CHECK(m.InvariantsHold());
}

std::string RandomString(std::mt19937_64& rng, std::size_t max_len)
{
    std::string s(rng() % (max_len + 1), 'a');
    for (char& c : s)
        c = static_cast<char>('a' + rng() % 4);
    return s;
}

template <typename Map, typename K, typename MakeKey>
void RunDifferential(std::uint64_t seed, int steps, MakeKey make_key)
{
    std::mt19937_64 rng(seed);
    Map m;
    Ref<K, std::string> ref;
    for (int step = 0; step < steps; ++step)
    {
        const K key = make_key(rng);
        const std::string val = RandomString(rng, 10);
        switch (rng() % 22)
        {
            case 0:
            case 1:
            {
                auto r = m.insert({key, val});
                CHECK(r.second == ref.Insert(key, val));
                CHECK(r.first->first == key && r.first->second == ref.index.at(key)->second);
                break;
            }
            case 2:
            case 3:
            {
                auto r = m.try_emplace(key, val);
                CHECK(r.second == ref.Insert(key, val));
                CHECK(r.first->second == ref.index.at(key)->second);
                break;
            }
            case 4:
            {
                auto r = m.emplace(key, val);
                CHECK(r.second == ref.Insert(key, val));
                break;
            }
            case 5:
            case 6:
            {
                // operator[]：已存在的不挪位置；赋值改值
                m[key] = val;
                if (!ref.Insert(key, val))
                    ref.index.at(key)->second = val;
                break;
            }
            case 7:
            {
                auto r = m.insert_or_assign(key, val);
                const bool inserted = ref.Insert(key, val);
                if (!inserted)
                    ref.index.at(key)->second = val;
                CHECK(r.second == inserted && r.first->second == val);
                break;
            }
            case 8:
            case 9:
            case 10:
            case 11:
                CHECK(m.erase(key) == (ref.Erase(key) ? 1u : 0u));
                break;
            case 12:
            {
                const auto a = m.find(key);
                const auto b = ref.index.find(key);
                CHECK((a == m.end()) == (b == ref.index.end()));
                if (b != ref.index.end())
                    CHECK(a->second == b->second->second);
                CHECK(m.contains(key) == (b != ref.index.end()));
                break;
            }
            case 13:
                if (!ref.order.empty())
                {
                    // 通过迭代器删，返回的"下一个"必须是顺序里的后继
                    const std::size_t pos = rng() % ref.order.size();
                    auto rit = ref.order.begin();
                    std::advance(rit, static_cast<std::ptrdiff_t>(pos));
                    auto mit = m.begin();
                    std::advance(mit, static_cast<std::ptrdiff_t>(pos));
                    CHECK(mit->first == rit->first);
                    const K victim = rit->first;
                    auto rnext = std::next(rit);
                    auto mnext = m.erase(mit);
                    ref.Erase(victim);
                    CHECK((mnext == m.end()) == (rnext == ref.order.end()));
                    if (mnext != m.end())
                        CHECK(mnext->first == rnext->first);
                }
                break;
            case 14:
                if (!ref.order.empty())
                {
                    const std::size_t pos = rng() % ref.order.size();
                    const std::size_t len = rng() % (ref.order.size() - pos + 1);
                    auto rfirst = ref.order.begin();
                    std::advance(rfirst, static_cast<std::ptrdiff_t>(pos));
                    auto rlast = rfirst;
                    std::advance(rlast, static_cast<std::ptrdiff_t>(len));
                    auto mfirst = m.begin();
                    std::advance(mfirst, static_cast<std::ptrdiff_t>(pos));
                    auto mlast = mfirst;
                    std::advance(mlast, static_cast<std::ptrdiff_t>(len));
                    const bool at_end = rlast == ref.order.end();
                    const K after = at_end ? K{} : rlast->first;
                    for (auto r = rfirst; r != rlast; ++r)
                        ref.index.erase(r->first);
                    ref.order.erase(rfirst, rlast);
                    auto next = m.erase(mfirst, mlast);
                    CHECK((next == m.end()) == at_end);
                    if (!at_end)
                        CHECK(next->first == after);
                }
                break;
            case 15:
                if (!ref.order.empty())
                {
                    const K k = ref.order.front().first;
                    CHECK(m.front().first == k);
                    m.pop_front();
                    ref.Erase(k);
                }
                break;
            case 16:
                if (!ref.order.empty())
                {
                    const K k = ref.order.back().first;
                    CHECK(m.back().first == k);
                    m.pop_back();
                    ref.Erase(k);
                }
                break;
            case 17:
                if (!ref.order.empty())
                {
                    const K k = ref.order.begin()->first;
                    const K target = (rng() % 2) ? k : ref.order.back().first;
                    const auto pick =
                        std::next(ref.order.begin(), static_cast<std::ptrdiff_t>(rng() % ref.order.size()));
                    const K chosen = (rng() % 3 == 0) ? target : pick->first;
                    if (rng() % 2)
                    {
                        m.move_to_back(m.find(chosen));
                        ref.MoveToBack(chosen);
                    }
                    else
                    {
                        m.move_to_front(m.find(chosen));
                        ref.MoveToFront(chosen);
                    }
                }
                break;
            case 18:
                if (rng() % 16 == 0)
                {
                    m.clear();
                    ref.Clear();
                }
                break;
            case 19:
                if (rng() % 8 == 0)
                {
                    if (rng() % 2)
                        m.reserve(m.size() + rng() % 300);
                    else
                        m.shrink_to_fit();
                }
                break;
            case 20:
                if (rng() % 8 == 0)
                {
                    Map copy(m);
                    CHECK(copy == m);
                    CompareOrdered(copy, ref); // 拷贝保持顺序
                    m = std::move(copy);
                }
                break;
            default:
                if (rng() % 8 == 0)
                {
                    Map other;
                    other.swap(m);
                    CHECK(m.empty() && m.InvariantsHold() && m.begin() == m.end());
                    m.swap(other);
                }
                break;
        }
        if (step % 61 == 0)
            CompareOrdered(m, ref);
    }
    CompareOrdered(m, ref);
}

void TestDifferential()
{
    using M = LinkedHashMap<int, std::string>;
    for (std::uint64_t seed = 1; seed <= 14; ++seed)
    {
        RunDifferential<M, int>(seed, 5000, [](std::mt19937_64& r) { return static_cast<int>(r() % 200); });
        RunDifferential<M, int>(seed + 100, 5000, [](std::mt19937_64& r) { return static_cast<int>(r() % 30000); });
    }
    using S = LinkedHashMap<std::string, std::string>;
    for (std::uint64_t seed = 1; seed <= 6; ++seed)
        RunDifferential<S, std::string>(seed, 4000, [](std::mt19937_64& r) { return RandomString(r, 30); });
    using P = LinkedHashMap<std::uint64_t, std::string, IdentityHash>;
    for (std::uint64_t seed = 1; seed <= 6; ++seed)
        RunDifferential<P, std::uint64_t>(seed, 4000, [](std::mt19937_64& r) { return (r() % 300) * 4096ULL; });
    using C = LinkedHashMap<int, std::string, ConstantHash>;
    for (std::uint64_t seed = 1; seed <= 4; ++seed)
        RunDifferential<C, int>(seed, 1500, [](std::mt19937_64& r) { return static_cast<int>(r() % 150); });
}

// ---------------- swap-and-pop 的边角 ----------------

LinkedHashMap<int, int> Make(std::initializer_list<int> keys)
{
    LinkedHashMap<int, int> m;
    for (const int k : keys)
        m[k] = k * 10;
    return m;
}

std::vector<int> Keys(const LinkedHashMap<int, int>& m)
{
    std::vector<int> out;
    for (const auto& [k, v] : m)
        out.push_back(k);
    return out;
}

void TestEraseReturnsCorrectNext()
{
    // 数组下标 0..4 依次是 1..5。删 4（下标 3）：它的后继 5 在数组最后，会被搬到下标 3 —— 返回的迭代器必须指向 5
    auto m = Make({1, 2, 3, 4, 5});
    auto next = m.erase(m.find(4));
    CHECK(next != m.end() && next->first == 5 && next->second == 50);
    CHECK((Keys(m) == std::vector<int>{1, 2, 3, 5}) && m.InvariantsHold());

    // 删最后一个：返回 end
    CHECK(m.erase(m.find(5)) == m.end());
    // 删第一个：后继 2，最后一个元素 3 搬到下标 0
    next = m.erase(m.find(1));
    CHECK(next->first == 2);
    CHECK((Keys(m) == std::vector<int>{2, 3}) && m.InvariantsHold());

    // 删到只剩一个、再删光
    CHECK(m.erase(m.find(2))->first == 3);
    CHECK(m.erase(m.find(3)) == m.end());
    CHECK(m.empty() && m.begin() == m.end() && m.InvariantsHold());
    m[9] = 90; // 删光之后可以接着用
    CHECK(Keys(m) == std::vector<int>{9});

    // 顺序与数组下标不一致时（move_to_back 之后），仍然正确
    auto n = Make({1, 2, 3, 4, 5});
    n.move_to_back(n.find(2));       // 顺序 1 3 4 5 2
    auto after = n.erase(n.find(5)); // 5 的后继是 2（数组下标 1）；数组最后一个是 5 自己
    CHECK(after->first == 2);
    CHECK((Keys(n) == std::vector<int>{1, 3, 4, 2}) && n.InvariantsHold());
    after = n.erase(n.find(1)); // 后继 3；数组最后一个元素（4 之后…）被搬动
    CHECK(after->first == 3);
    CHECK((Keys(n) == std::vector<int>{3, 4, 2}) && n.InvariantsHold());
}

void TestEraseRangeWhenEndIsMoved()
{
    // 顺序 a b c d e，删 [b, e)：删 b 时数组最后一个元素 e（正是区间末端）被搬进 b 的位置，区间末端的下标要跟着改
    auto m = Make({1, 2, 3, 4, 5});
    auto next = m.erase(m.find(2), m.find(5));
    CHECK(next != m.end() && next->first == 5);
    CHECK((Keys(m) == std::vector<int>{1, 5}) && m.InvariantsHold());

    // 区间到末尾
    auto n = Make({1, 2, 3, 4, 5});
    CHECK(n.erase(n.find(3), n.end()) == n.end());
    CHECK((Keys(n) == std::vector<int>{1, 2}) && n.InvariantsHold());
    // 空区间、全区间
    auto p = Make({1, 2, 3});
    CHECK(p.erase(p.find(2), p.find(2))->first == 2 && p.size() == 3);
    CHECK(p.erase(p.begin(), p.end()) == p.end() && p.empty() && p.InvariantsHold());

    // 随机的区间删除：与参考实现一致（RunDifferential 的 case 14 也覆盖，这里专门多跑）
    std::mt19937_64 rng(5);
    for (int round = 0; round < 3000; ++round)
    {
        const int n_elems = 1 + static_cast<int>(rng() % 12);
        LinkedHashMap<int, int> t;
        std::vector<int> order;
        for (int i = 0; i < n_elems; ++i)
        {
            t[i] = i;
            order.push_back(i);
        }
        if (rng() % 2) // 打乱数组下标与顺序的对应
        {
            const int k = static_cast<int>(rng() % static_cast<std::uint64_t>(n_elems));
            t.move_to_back(t.find(k));
            order.erase(std::find(order.begin(), order.end(), k));
            order.push_back(k);
        }
        const std::size_t a = rng() % (order.size() + 1);
        const std::size_t b = a + rng() % (order.size() - a + 1);
        auto first = t.begin();
        std::advance(first, static_cast<std::ptrdiff_t>(a));
        auto last = t.begin();
        std::advance(last, static_cast<std::ptrdiff_t>(b));
        auto ret = t.erase(first, last);
        order.erase(order.begin() + static_cast<std::ptrdiff_t>(a), order.begin() + static_cast<std::ptrdiff_t>(b));
        CHECK(Keys(t) == order && t.InvariantsHold());
        if (a < order.size())
            CHECK(ret->first == order[a]);
        else
            CHECK(ret == t.end());
    }
}

void TestEraseWhileIterating()
{
    LinkedHashMap<int, int> m;
    for (int i = 0; i < 1000; ++i)
        m[i] = i;
    std::vector<int> visited;
    for (auto it = m.begin(); it != m.end();)
    {
        visited.push_back(it->first);
        if (it->first % 3 == 0)
            it = m.erase(it);
        else
            ++it;
    }
    CHECK(visited.size() == 1000); // 每个元素都被访问到、且只访问一次
    for (int i = 0; i < 1000; ++i)
        CHECK(visited[static_cast<std::size_t>(i)] == i);
    CHECK(m.size() == 666 && m.InvariantsHold());

    CHECK(erase_if(m, [](const auto& kv) { return kv.first % 2 == 0; }) > 0);
    for (const auto& kv : m)
        CHECK(kv.first % 2 == 1 && kv.first % 3 != 0);

    LinkedHashSet<int> s;
    for (int i = 0; i < 100; ++i)
        s.insert(i);
    CHECK(erase_if(s, [](int v) { return v < 50; }) == 50);
    CHECK(*s.begin() == 50 && s.InvariantsHold());
}

// ---------------- 移动 / 拷贝 ----------------

void TestMovedFromIsFullyEmpty()
{
    auto a = Make({1, 2, 3, 4, 5, 6, 7, 8, 9, 10});
    auto b(std::move(a));
    CHECK(b.size() == 10 && b.InvariantsHold());
    // 旧实现（默认的移动）只搬走了数组与桶，链头尾仍是旧值：begin() != end()，再插入就越界
    CHECK(a.empty() && a.size() == 0 && a.begin() == a.end() && a.rbegin() == a.rend() && a.InvariantsHold()); // NOLINT
    CHECK(a.capacity() == 0);                                                                                  // NOLINT
    a[100] = 1;                                                                                                // NOLINT
    a[101] = 2;                                                                                                // NOLINT
    CHECK((Keys(a) == std::vector<int>{100, 101}) && a.InvariantsHold());                                      // NOLINT

    // 移动赋值
    LinkedHashMap<int, int> c;
    c[77] = 7;
    c = std::move(b);
    CHECK(c.size() == 10 && !c.contains(77) && c.InvariantsHold());
    CHECK(b.empty() && b.begin() == b.end() && b.InvariantsHold()); // NOLINT
    b[1] = 1;                                                       // NOLINT
    CHECK(b.size() == 1 && b.InvariantsHold());                     // NOLINT

    // 自赋值 / 自移动赋值
    auto& cr = c;
    c = cr;
    CHECK(c.size() == 10 && c.InvariantsHold());
    c = std::move(cr);
    CHECK(c.size() == 10 && c.InvariantsHold());
}

void TestCopyKeepsOrderAndIsIndependent()
{
    LinkedHashMap<std::string, int> a;
    for (const char* k : {"delta", "alpha", "charlie", "bravo", "echo"})
        a[k] = static_cast<int>(std::string_view(k).size());
    a.move_to_front(a.find("echo"));
    a.erase("charlie");
    const std::vector<std::string> expect{"echo", "delta", "alpha", "bravo"};
    auto order = [](const LinkedHashMap<std::string, int>& m)
    {
        std::vector<std::string> v;
        for (const auto& [k, _] : m)
            v.push_back(k);
        return v;
    };
    CHECK(order(a) == expect);

    LinkedHashMap<std::string, int> b(a);
    CHECK(order(b) == expect && b == a && b.InvariantsHold());
    b["zulu"] = 1;
    b.erase("delta");
    CHECK(order(a) == expect && a.contains("delta") && !a.contains("zulu"));
    CHECK(a != b);

    LinkedHashMap<std::string, int> c;
    c = a;
    CHECK(order(c) == expect && c.InvariantsHold());

    // 相等与顺序无关
    LinkedHashMap<int, int> p{{1, 1}, {2, 2}, {3, 3}};
    LinkedHashMap<int, int> q{{3, 3}, {1, 1}, {2, 2}};
    CHECK(p == q);
    CHECK(Keys(p) != Keys(q));

    // 初始化列表（重复的键保留第一个）、区间构造、列表赋值
    LinkedHashMap<int, int> il{{5, 50}, {1, 10}, {5, 99}, {3, 30}};
    CHECK((Keys(il) == std::vector<int>{5, 1, 3}) && il.at(5) == 50);
    std::vector<std::pair<int, int>> v{{9, 90}, {8, 80}};
    LinkedHashMap<int, int> r(v.begin(), v.end());
    CHECK(Keys(r) == (std::vector<int>{9, 8}));
    r = {{4, 40}};
    CHECK(Keys(r) == std::vector<int>{4});

    LinkedHashSet<int> s{3, 1, 2, 3};
    CHECK((std::vector<int>(s.begin(), s.end()) == std::vector<int>{3, 1, 2}));
    LinkedHashSet<int> s2(s);
    s2.insert(0);
    CHECK(s.size() == 3 && s2.size() == 4 && s == (LinkedHashSet<int>{1, 2, 3}));
}

// ---------------- 顺序规则 ----------------

void TestOrderRules()
{
    LinkedHashMap<int, int> m;
    m[3] = 1;
    m[1] = 1;
    m[2] = 1;
    // 重复插入 / 赋值 / operator[] 都不改变位置
    m[3] = 99;
    m.insert_or_assign(1, 5);
    m.try_emplace(2, 7);
    m.emplace(3, 0);
    m.insert({1, 0});
    CHECK((Keys(m) == std::vector<int>{3, 1, 2}));
    CHECK(m.at(3) == 99 && m.at(1) == 5 && m.at(2) == 1);

    // 删了再插 = 追加到末尾
    m.erase(1);
    m[1] = 0;
    CHECK((Keys(m) == std::vector<int>{3, 2, 1}));

    // move_to_back / move_to_front
    m.move_to_back(m.find(3));
    CHECK((Keys(m) == std::vector<int>{2, 1, 3}));
    m.move_to_front(m.find(3));
    CHECK((Keys(m) == std::vector<int>{3, 2, 1}));
    m.move_to_back(m.find(1)); // 已经在末尾：什么也不变
    m.move_to_front(m.find(3));
    CHECK((Keys(m) == std::vector<int>{3, 2, 1}) && m.InvariantsHold());
    // 挪动之后迭代器 / 引用仍然有效（元素没动，只改了链）
    auto it = m.find(2);
    int* ref = &it->second;
    m.move_to_back(it);
    CHECK(&it->second == ref && it->first == 2);
    CHECK(m.back().first == 2 && m.front().first == 3);

    // 反向迭代与 --end()
    auto e = m.end();
    --e;
    CHECK(e->first == 2);
    --e;
    CHECK(e->first == 1);
    std::vector<int> rev;
    for (auto r = m.rbegin(); r != m.rend(); ++r)
        rev.push_back(r->first);
    CHECK((rev == std::vector<int>{2, 1, 3}));

    // pop_front / pop_back
    m.pop_front();
    m.pop_back();
    CHECK((Keys(m) == std::vector<int>{1}) && m.InvariantsHold());

    LinkedHashSet<int> s;
    for (int i = 0; i < 5; ++i)
        s.insert(i);
    s.insert(2); // 已有：位置不变
    s.move_to_back(s.find(0));
    CHECK((std::vector<int>(s.begin(), s.end()) == std::vector<int>{1, 2, 3, 4, 0}));
    s.pop_front();
    CHECK(*s.begin() == 2 && s.back() == 0 && s.front() == 2);
}

// ---------------- LRU ----------------

// 独立的 LRU：list（队首最近使用）+ unordered_map。被测实现用 move_to_back / pop_front（队尾最近使用）。
struct RefLru
{
    std::size_t cap;
    std::list<std::pair<int, int>> order; // 队首 = 最近使用
    std::unordered_map<int, std::list<std::pair<int, int>>::iterator> index;
    bool Get(int k, int& out)
    {
        auto it = index.find(k);
        if (it == index.end())
            return false;
        order.splice(order.begin(), order, it->second);
        out = it->second->second;
        return true;
    }
    void Put(int k, int v)
    {
        auto it = index.find(k);
        if (it != index.end())
        {
            it->second->second = v;
            order.splice(order.begin(), order, it->second);
            return;
        }
        order.emplace_front(k, v);
        index[k] = order.begin();
        if (order.size() > cap)
        {
            index.erase(order.back().first);
            order.pop_back();
        }
    }
};

void TestLru()
{
    constexpr std::size_t kCap = 50;
    LinkedHashMap<int, int> lru;
    RefLru ref{kCap, {}, {}};
    std::mt19937_64 rng(31);
    for (int i = 0; i < 100000; ++i)
    {
        const int k = static_cast<int>(rng() % 120);
        if (rng() % 2)
        {
            int expect = -1, got = -1;
            const bool hit = ref.Get(k, expect);
            const auto it = lru.find(k);
            CHECK((it != lru.end()) == hit);
            if (it != lru.end())
            {
                got = it->second;
                lru.move_to_back(it); // 命中：标记为最近使用
                CHECK(got == expect);
            }
        }
        else
        {
            const int v = static_cast<int>(rng() % 1000);
            ref.Put(k, v);
            const auto [it, inserted] = lru.insert_or_assign(k, v);
            if (!inserted)
                lru.move_to_back(it);
            if (lru.size() > kCap)
                lru.pop_front(); // 淘汰最久没用的
        }
        CHECK(lru.size() == ref.order.size());
    }
    // 最终顺序一致：被测实现队尾 = 最近使用，参考实现队首 = 最近使用
    auto it = lru.rbegin();
    for (const auto& [k, v] : ref.order)
    {
        CHECK(it != lru.rend() && it->first == k && it->second == v);
        ++it;
    }
    CHECK(lru.InvariantsHold());
}

// ---------------- 类型与语义 ----------------

void TestStatefulHasher()
{
    using M = LinkedHashMap<int, int, SeededHash>;
    M m(0, SeededHash{4242});
    for (int i = 0; i < 3000; ++i)
        m[i] = i;
    for (int i = 0; i < 3000; ++i)
        CHECK(m.at(i) == i);
    M copy(m);
    CHECK(copy.hash_function().seed == 4242 && copy == m);
    M moved(std::move(copy));
    CHECK(moved.hash_function().seed == 4242 && moved == m);
    M other(0, SeededHash{9});
    other[1] = 1;
    other.swap(moved);
    CHECK(other.hash_function().seed == 4242 && moved.hash_function().seed == 9);
    CHECK(other == m && moved.at(1) == 1);
    for (int i = 0; i < 2900; ++i)
        m.erase(i);
    m.shrink_to_fit();
    for (int i = 2900; i < 3000; ++i)
        CHECK(m.at(i) == i);
    CHECK(m.InvariantsHold());
}

void TestMoveOnlyValues()
{
    LinkedHashMap<int, std::unique_ptr<int>> m;
    for (int i = 0; i < 2000; ++i)
        m.try_emplace(i, std::make_unique<int>(i * 7));
    for (int i = 0; i < 2000; i += 2)
        m.erase(i);
    int expect = 1;
    for (const auto& [k, v] : m)
    {
        CHECK(k == expect && *v == expect * 7);
        expect += 2;
    }
    auto p = std::make_unique<int>(1);
    CHECK(!m.try_emplace(1, std::move(p)).second);
    CHECK(p != nullptr); // NOLINT：键已存在，参数不动
    auto moved = std::move(m);
    CHECK(moved.size() == 1000 && m.empty()); // NOLINT
}

struct alignas(64) Aligned64
{
    std::uint64_t v = 0;
};

void TestOverAligned()
{
    LinkedHashMap<int, Aligned64> m;
    for (int i = 0; i < 1000; ++i)
        m[i].v = static_cast<std::uint64_t>(i);
    for (int i = 0; i < 1000; i += 3)
        m.erase(i);
    for (auto& [k, v] : m)
    {
        CHECK(reinterpret_cast<std::uintptr_t>(&v) % 64 == 0);
        CHECK(v.v == static_cast<std::uint64_t>(k));
    }
    CHECK(m.InvariantsHold());
}

void TestLifetimeAccounting()
{
    g_c = {};
    {
        LinkedHashMap<Tracked, Tracked, TrackedHash> m;
        for (int i = 0; i < 3000; ++i)
            m.try_emplace(Tracked(i), Tracked(i));
        CHECK(g_c.copies == 0); // 数组扩容只移动
        for (int i = 0; i < 3000; i += 2)
            m.erase(Tracked(i)); // swap-and-pop 搬元素用移动
        CHECK(g_c.copies == 0);
        CHECK(g_c.live == 2 * 1500);
        m.shrink_to_fit();
        CHECK(g_c.copies == 0 && g_c.live == 2 * 1500);
        LinkedHashMap<Tracked, Tracked, TrackedHash> copy(m);
        CHECK(g_c.live == 2 * 3000 && g_c.copies == 2 * 1500);
        m.clear();
        CHECK(g_c.live == 2 * 1500);
        m = copy;
        CHECK(g_c.live == 2 * 3000);
        m.pop_front();
        m.pop_back();
        CHECK(g_c.live == 2 * 2998); // 两个表各 1500 个条目，m 少了两个条目（每个条目一个键一个值）
    }
    CHECK(g_c.live == 0);
    g_c = {};
    {
        LinkedHashSet<Tracked, TrackedHash> s;
        for (int i = 0; i < 1000; ++i)
            s.emplace(i);
        CHECK(g_c.copies == 0);
        LinkedHashSet<Tracked, TrackedHash> s2(std::move(s));
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
    ExerciseCommonApiForms<LinkedHashMap<std::string, int>>();
    ExerciseCommonSetForms<LinkedHashSet<std::string>>();
}

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

// 元素构造抛异常：表里什么也没登记，仍然有效、顺序不变、可以继续用；拷贝整个表中途抛不泄漏。
void TestConstructionThrowing()
{
    LinkedHashMap<int, Thrower> m;
    for (int i = 0; i < 100; ++i)
        m.try_emplace(i, Thrower(i, false));
    const Thrower bad(1000, true);
    bool threw = false;
    try
    {
        m.try_emplace(1000, bad);
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }
    CHECK(threw && m.size() == 100 && !m.contains(1000) && m.InvariantsHold() && m.back().first == 99);

    // 数组恰好满：扩容完成后新元素构造抛
    LinkedHashMap<int, Thrower> full;
    full.reserve(8);
    for (int i = 0; i < 8; ++i)
        full.try_emplace(i, Thrower(i, false));
    CHECK(full.capacity() == 8);
    threw = false;
    try
    {
        full.try_emplace(999, bad);
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }
    CHECK(threw && full.size() == 8 && !full.contains(999) && full.InvariantsHold());
    full.try_emplace(998, Thrower(998, false));
    CHECK(full.back().first == 998 && full.InvariantsHold());

    LinkedHashMap<int, Thrower> with_bad;
    for (int i = 0; i < 50; ++i)
        with_bad.try_emplace(i, Thrower(i, false));
    with_bad.at(25).boom = true;
    threw = false;
    try
    {
        LinkedHashMap<int, Thrower> copy(with_bad);
        (void)copy;
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }
    CHECK(threw && with_bad.size() == 50 && with_bad.InvariantsHold());
}

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
        ++g_c.copies;
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
    LinkedHashMap<CountingKey, int, TransparentHash, TransparentEq> m;
    for (int i = 0; i < 300; ++i)
        m.try_emplace(CountingKey("k" + std::to_string(i)), i);
    g_c = {};
    for (int i = 0; i < 300; ++i)
    {
        const std::string name = "k" + std::to_string(i);
        const KeyView kv{name};
        CHECK(m.contains(kv) && m.count(kv) == 1 && m.find(kv)->second == i && m.at(kv) == i);
    }
    CHECK(!m.contains(KeyView{"nope"}));
    CHECK(m.erase(KeyView{"k5"}) == 1 && m.erase(KeyView{"k5"}) == 0);
    CHECK(g_c.copies == 0); // 没有为了查找构造临时的 CountingKey

    LinkedHashMap<std::string, int, StringHash, std::equal_to<>> names;
    names["alice"] = 1;
    CHECK(names.contains(std::string_view("alice")) && names.at(std::string_view("alice")) == 1);
    LinkedHashSet<std::string, StringHash, std::equal_to<>> set;
    set.insert("x");
    CHECK(set.contains(std::string_view("x")) && set.erase(std::string_view("x")) == 1);
    LinkedHashMap<std::string, int> plain;
    plain["abc"] = 1;
    CHECK(plain.contains("abc") && plain.erase("abc") == 1);
}

static_assert(std::is_const_v<std::remove_reference_t<decltype(*std::declval<LinkedHashSet<int>&>().begin())>>);
static_assert(
    std::is_const_v<std::remove_reference_t<decltype(((*std::declval<LinkedHashMap<int, int>&>().begin()).first))>>);
static_assert(
    !std::is_const_v<std::remove_reference_t<decltype(((*std::declval<LinkedHashMap<int, int>&>().begin()).second))>>);
static_assert(std::is_same_v<std::iterator_traits<LinkedHashMap<int, int>::iterator>::iterator_category,
                             std::bidirectional_iterator_tag>);

void TestSetBasics()
{
    LinkedHashSet<std::string> s;
    CHECK(s.insert("b").second && s.insert("a").second && !s.insert("b").second);
    CHECK(s.emplace("c").second && !s.emplace("a").second);
    CHECK((std::vector<std::string>(s.begin(), s.end()) == std::vector<std::string>{"b", "a", "c"}));
    CHECK(s.erase("a") == 1 && s.erase("zzz") == 0);
    CHECK((std::vector<std::string>(s.begin(), s.end()) == std::vector<std::string>{"b", "c"}) && s.InvariantsHold());
    // 已存在的右值键不被移走
    std::string dup = "b";
    CHECK(!s.insert(std::move(dup)).second);
    CHECK(dup == "b"); // NOLINT
    std::string fresh = "zz";
    CHECK(s.insert(std::move(fresh)).second);
    CHECK(fresh.empty()); // NOLINT：被移进表里
}

void TestAtMissingAborts()
{
    std::fflush(nullptr);
    const pid_t pid = ::fork();
    CHECK(pid >= 0);
    if (pid == 0)
    {
        std::freopen("/dev/null", "w", stderr);
        LinkedHashMap<int, int> m;
        m[1] = 1;
        (void)m.at(2);
        ::_exit(0);
    }
    int status = 0;
    CHECK(::waitpid(pid, &status, 0) == pid);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    LinkedHashMap<int, int> ok;
    ok[1] = 11;
    const auto& cok = ok;
    CHECK(ok.at(1) == 11 && cok.at(1) == 11);
}

void TestCapacityAndReserve()
{
    LinkedHashMap<int, int> m;
    CHECK(m.capacity() == 0 && m.empty() && m.begin() == m.end() && m.InvariantsHold());
    CHECK(!m.contains(1) && m.find(1) == m.end() && m.erase(1) == 0);
    m.clear();
    m.shrink_to_fit();
    m.reserve(1000);
    CHECK(m.capacity() >= 1000);
    const std::size_t cap = m.capacity();
    const int* addr = nullptr;
    for (int i = 0; i < 1000; ++i)
    {
        m[i] = i;
        if (i == 0)
            addr = &m.find(0)->second;
    }
    CHECK(m.capacity() == cap && &m.find(0)->second == addr); // reserve 之后放 n 个以内不扩容、不搬动
    for (int i = 0; i < 990; ++i)
        m.erase(i);
    m.shrink_to_fit();
    CHECK(m.capacity() == 10 && m.InvariantsHold());
    for (int i = 990; i < 1000; ++i)
        CHECK(m.at(i) == i);
    m.clear();
    CHECK(m.empty() && m.capacity() == 10 && m.InvariantsHold()); // clear 保留容量
    m.shrink_to_fit();
    CHECK(m.capacity() == 0 && m.InvariantsHold());
}

void TestLargeScale()
{
    constexpr int kN = 200000;
    LinkedHashMap<std::uint64_t, int> m;
    std::vector<std::uint64_t> keys;
    std::mt19937_64 rng(11);
    for (int i = 0; i < kN; ++i)
    {
        keys.push_back(rng());
        m[keys.back()] = i;
    }
    CHECK(m.size() == static_cast<std::size_t>(kN) && m.InvariantsHold());
    // 顺序就是插入顺序
    int idx = 0;
    for (const auto& [k, v] : m)
    {
        CHECK(k == keys[static_cast<std::size_t>(idx)] && v == idx);
        ++idx;
    }
    // 随机删一半，剩下的相对顺序不变
    std::vector<bool> gone(static_cast<std::size_t>(kN), false);
    for (int i = 0; i < kN; ++i)
    {
        if (rng() % 2)
        {
            CHECK(m.erase(keys[static_cast<std::size_t>(i)]) == 1);
            gone[static_cast<std::size_t>(i)] = true;
        }
    }
    CHECK(m.InvariantsHold());
    auto it = m.begin();
    for (int i = 0; i < kN; ++i)
    {
        if (gone[static_cast<std::size_t>(i)])
            continue;
        CHECK(it != m.end() && it->first == keys[static_cast<std::size_t>(i)] && it->second == i);
        ++it;
    }
    CHECK(it == m.end());
}

} // namespace

int main()
{
    TestDifferential();
    TestEraseReturnsCorrectNext();
    TestEraseRangeWhenEndIsMoved();
    TestEraseWhileIterating();
    TestMovedFromIsFullyEmpty();
    TestCopyKeepsOrderAndIsIndependent();
    TestOrderRules();
    TestLru();
    TestStatefulHasher();
    TestMoveOnlyValues();
    TestOverAligned();
    TestLifetimeAccounting();
    TestCommonApiForms();
    TestConstructionThrowing();
    TestHeterogeneousLookup();
    TestSetBasics();
    TestAtMissingAborts();
    TestCapacityAndReserve();
    TestLargeScale();
    std::puts("linked_hash_test: ok");
    return 0;
}
