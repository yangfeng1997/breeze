// 容器基准：FlatHashMap / LinkedHashMap 对比 std::unordered_map。
// 构建：cmake --preset release -DBREEZE_BUILD_CONTAINER_BENCH=ON；运行：container_bench [元素数 ...]（默认 1000 100000
// 1000000）。 每个场景重复几次取最快的一次，输出每次操作的纳秒数；内存按"插入完成前后堆使用量之差 /
// 元素数"估算（mallinfo2）。 结果受机器与 CPU 频率影响很大，只看相对比例；跑的时候最好绑核（taskset -c 2）。

#include <malloc.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "flat_hash_map.h"
#include "linked_hash_map.h"

using Clock = std::chrono::steady_clock;

namespace
{

template <typename F>
double BestNsPerOp(int repeats, std::size_t ops, F&& f)
{
    double best = 1e30;
    for (int r = 0; r < repeats; ++r)
    {
        const auto t0 = Clock::now();
        f();
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count();
        best = std::min(best, static_cast<double>(ns) / static_cast<double>(ops));
    }
    return best;
}

std::size_t HeapInUse() { return mallinfo2().uordblks + mallinfo2().hblkhd; }

// 防止优化器把查找结果丢掉
std::uint64_t g_sink = 0;

// keys：插入顺序；probes：命中查找与删除的顺序（keys 的一个排列）；misses：一批不在表里的键。
template <typename Map, typename Keys>
void Run(const char* name, const Keys& keys, const Keys& probes, const Keys& misses)
{
    const std::size_t n = keys.size();
    const int repeats = n >= 1000000 ? 3 : 5;

    // 插入（从空表开始，含全部扩容）
    const double insert_ns = BestNsPerOp(repeats,
                                         n,
                                         [&]
                                         {
                                             Map m;
                                             for (std::size_t i = 0; i < n; ++i)
                                                 m[keys[i]] = i;
                                             g_sink += m.size();
                                         });

    // 预先 reserve 的插入
    double insert_reserved_ns = 0;
    if constexpr (requires(Map m) { m.reserve(std::size_t{}); })
    {
        insert_reserved_ns = BestNsPerOp(repeats,
                                         n,
                                         [&]
                                         {
                                             Map m;
                                             m.reserve(n);
                                             for (std::size_t i = 0; i < n; ++i)
                                                 m[keys[i]] = i;
                                             g_sink += m.size();
                                         });
    }

    const std::size_t before = HeapInUse();
    Map m;
    for (std::size_t i = 0; i < n; ++i)
        m[keys[i]] = i;
    const double bytes_per = static_cast<double>(HeapInUse() - before) / static_cast<double>(n);

    const double hit_ns = BestNsPerOp(repeats,
                                      n,
                                      [&]
                                      {
                                          std::uint64_t s = 0;
                                          for (std::size_t i = 0; i < n; ++i)
                                              s += m.find(probes[i])->second;
                                          g_sink += s;
                                      });
    const double miss_ns = BestNsPerOp(repeats,
                                       n,
                                       [&]
                                       {
                                           std::uint64_t s = 0;
                                           for (std::size_t i = 0; i < n; ++i)
                                               s += m.find(misses[i]) == m.end();
                                           g_sink += s;
                                       });
    const double iter_ns = BestNsPerOp(repeats,
                                       n,
                                       [&]
                                       {
                                           std::uint64_t s = 0;
                                           for (const auto& kv : m)
                                               s += kv.second;
                                           g_sink += s;
                                       });

    // 删除全部（逐个按键删）
    double erase_ns = 1e30;
    for (int r = 0; r < repeats; ++r)
    {
        Map t;
        for (std::size_t i = 0; i < n; ++i)
            t[keys[i]] = i;
        const auto t0 = Clock::now();
        for (std::size_t i = 0; i < n; ++i)
            g_sink += t.erase(probes[i]);
        erase_ns = std::min(
            erase_ns,
            static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count()) /
                static_cast<double>(n));
    }

    // 高装载下的"删一个插一个"：稳态 n 个元素
    std::mt19937_64 rng(5);
    std::vector<std::size_t> victims(n);
    for (auto& v : victims)
        v = rng() % n;
    const double churn_ns = BestNsPerOp(repeats,
                                        n,
                                        [&]
                                        {
                                            Map t;
                                            for (std::size_t i = 0; i < n; ++i)
                                                t[keys[i]] = i;
                                            for (std::size_t i = 0; i < n; ++i)
                                            {
                                                const std::size_t v = victims[i];
                                                if (t.erase(keys[v]) == 0)
                                                    t[keys[v]] = i;
                                                else
                                                    t[misses[v]] = i; // 换一个新键进去，保持元素数
                                            }
                                            g_sink += t.size();
                                        });

    std::printf(
        "  %-26s insert %7.1f  insert(reserve) %7.1f  hit %7.1f  miss %7.1f  iterate %6.1f  erase %7.1f  churn %7.1f  "
        "%6.1f B/elem\n",
        name,
        insert_ns,
        insert_reserved_ns,
        hit_ns,
        miss_ns,
        iter_ns,
        erase_ns,
        churn_ns,
        bytes_per);
}

} // namespace

int main(int argc, char** argv)
{
    std::vector<std::size_t> sizes;
    for (int i = 1; i < argc; ++i)
        sizes.push_back(static_cast<std::size_t>(std::atoll(argv[i])));
    if (sizes.empty())
        sizes = {1000, 100000, 1000000};

    for (const std::size_t n : sizes)
    {
        std::mt19937_64 rng(42);
        std::vector<std::uint64_t> keys(n), misses(n);
        for (auto& k : keys)
            k = rng();
        for (auto& k : misses)
            k = rng();
        std::printf("== uint64 -> uint64, n = %zu (ns/op)\n", n);
        Run<std::unordered_map<std::uint64_t, std::uint64_t>>("std::unordered_map", keys, keys, misses);
        Run<breeze::FlatHashMap<std::uint64_t, std::uint64_t>>("breeze::FlatHashMap", keys, keys, misses);
        Run<breeze::LinkedHashMap<std::uint64_t, std::uint64_t>>("breeze::LinkedHashMap", keys, keys, misses);

        // 连续 id：玩家 / 对象 id 的常见形态。恒等哈希 + 按插入顺序访问是 std::unordered_map 的最好情形
        // （桶号连续、节点按插入顺序分配，访问有很好的局部性）；真实负载里谁来访问是随机的，所以再给一组打乱访问顺序的。
        std::vector<std::uint64_t> seq(n), seq_miss(n);
        for (std::size_t i = 0; i < n; ++i)
        {
            seq[i] = 1000000 + i;
            seq_miss[i] = 1000000000 + i;
        }
        std::vector<std::uint64_t> seq_shuffled = seq;
        std::shuffle(seq_shuffled.begin(), seq_shuffled.end(), rng);
        std::printf("== uint64 sequential ids, access in insertion order, n = %zu (ns/op)\n", n);
        Run<std::unordered_map<std::uint64_t, std::uint64_t>>("std::unordered_map", seq, seq, seq_miss);
        Run<breeze::FlatHashMap<std::uint64_t, std::uint64_t>>("breeze::FlatHashMap", seq, seq, seq_miss);
        Run<breeze::LinkedHashMap<std::uint64_t, std::uint64_t>>("breeze::LinkedHashMap", seq, seq, seq_miss);
        std::printf("== uint64 sequential ids, random access order, n = %zu (ns/op)\n", n);
        Run<std::unordered_map<std::uint64_t, std::uint64_t>>("std::unordered_map", seq, seq_shuffled, seq_miss);
        Run<breeze::FlatHashMap<std::uint64_t, std::uint64_t>>("breeze::FlatHashMap", seq, seq_shuffled, seq_miss);
        Run<breeze::LinkedHashMap<std::uint64_t, std::uint64_t>>("breeze::LinkedHashMap", seq, seq_shuffled, seq_miss);

        std::vector<std::string> skeys(n), smiss(n);
        for (std::size_t i = 0; i < n; ++i)
        {
            skeys[i] = "player_name_" + std::to_string(rng() % 100000000000ULL) + "_x"; // 超出 SSO，堆分配
            smiss[i] = "missing_name_" + std::to_string(rng() % 100000000000ULL) + "_x";
        }
        std::printf("== string (~28 bytes) -> uint64, n = %zu (ns/op)\n", n);
        Run<std::unordered_map<std::string, std::uint64_t>>("std::unordered_map", skeys, skeys, smiss);
        Run<breeze::FlatHashMap<std::string, std::uint64_t>>("breeze::FlatHashMap", skeys, skeys, smiss);
        Run<breeze::LinkedHashMap<std::string, std::uint64_t>>("breeze::LinkedHashMap", skeys, skeys, smiss);
    }
    return g_sink == 1 ? 1 : 0;
}
