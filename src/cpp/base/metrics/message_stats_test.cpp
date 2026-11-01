// MessageStats：默认关闭、按 id / 名字累加、排序、max_keys 上限与 dropped 守恒、报告格式（含 top_n 折叠）、
// Tick 的窗口与"先清零再回调"、默认写日志、与 std::map 参考实现的随机对照。
// 日志写到 stderr（没有默认 logger 时的兜底），用重定向 fd 2 的办法读回来。无测试框架依赖：失败即 abort。

#include "base/metrics/message_stats.h"

#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <string_view>
#include <tuple>
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

using Counter = MessageStats::Counter;

bool Same(const Counter& a, const Counter& b) { return a.count == b.count && a.bytes == b.bytes; }
bool Contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

// 在当前进程里跑 fn，期间 fd 2 指向临时文件，返回这段时间写到 stderr 的内容。
std::string CaptureStderr(const std::function<void()>& fn)
{
    std::fflush(stderr);
    char path[] = "/tmp/breeze_message_stats_test_XXXXXX";
    const int tmp = ::mkstemp(path);
    CHECK(tmp >= 0);
    const int saved = ::dup(STDERR_FILENO);
    ::dup2(tmp, STDERR_FILENO);
    fn();
    std::fflush(stderr);
    ::dup2(saved, STDERR_FILENO);
    ::close(saved);
    ::lseek(tmp, 0, SEEK_SET);
    std::string out;
    char buf[4096];
    for (ssize_t n; (n = ::read(tmp, buf, sizeof(buf))) > 0;)
        out.append(buf, static_cast<std::size_t>(n));
    ::close(tmp);
    ::unlink(path);
    return out;
}

// 关着的时候 Add 什么都不记；Enable 之后才开始。
void TestDisabledByDefault()
{
    MessageStats s("t");
    CHECK(!s.enabled());
    s.Add(1, 100);
    s.Add("Foo", 100);
    auto snap = s.GetSnapshot();
    CHECK(snap.by_id.empty() && snap.by_name.empty() && snap.dropped.count == 0);

    s.Enable();
    s.Enable(); // 重复打开是幂等的
    CHECK(s.enabled());
    s.Add(1, 100);
    snap = s.GetSnapshot();
    CHECK(snap.by_id.size() == 1 && snap.by_name.empty());
}

void TestCountsAndBytes()
{
    MessageStats s("t");
    s.Enable();
    s.Add(1001, 10);
    s.Add(1001, 20);
    s.Add(1001, 0); // 空消息也算一条
    s.Add(7, 5);
    s.Add(0, 1); // 字面量 0 要选 id 重载，而不是 string_view(nullptr)
    s.Add("PlayerMove", 100);
    s.Add("PlayerMove", 50);
    s.Add("5", 9); // 名字 "5" 与 id 5 不是同一项
    s.Add(5, 1);
    s.Add("", 3); // 空名字是合法的键
    const std::string with_nul("a\0b", 3);
    s.Add(with_nul, 4);
    s.Add(std::string("a"), 2); // 与带 NUL 的不是同一个键

    const auto snap = s.GetSnapshot();
    std::map<std::uint32_t, Counter> ids;
    for (const auto& e : snap.by_id)
        ids[e.id] = e.total;
    CHECK(ids.size() == 4);
    CHECK(Same(ids[1001], {3, 30}) && Same(ids[7], {1, 5}) && Same(ids[0], {1, 1}) && Same(ids[5], {1, 1}));

    std::map<std::string, Counter> names;
    for (const auto& e : snap.by_name)
        names[e.name] = e.total;
    CHECK(names.size() == 5);
    CHECK(Same(names["PlayerMove"], {2, 150}) && Same(names["5"], {1, 9}) && Same(names[""], {1, 3}));
    CHECK(Same(names[with_nul], {1, 4}) && Same(names["a"], {1, 2}));
    CHECK(snap.dropped.count == 0);
}

// count 降序，再 bytes 降序，再键升序。
void TestSnapshotOrder()
{
    MessageStats s("t");
    s.Enable();
    s.Add(30, 1);
    s.Add(10, 5);
    s.Add(20, 5);
    s.Add(40, 1);
    s.Add(40, 1); // count 2，排最前
    s.Add(50, 9); // count 1 但 bytes 最大，排在 count 1 里最前
    const auto a = s.GetSnapshot();
    std::vector<std::uint32_t> got;
    for (const auto& e : a.by_id)
        got.push_back(e.id);
    CHECK((got == std::vector<std::uint32_t>{40, 50, 10, 20, 30}));

    s.Add("b", 1);
    s.Add("a", 1);
    s.Add("c", 2);
    s.Add("c", 2);
    const auto b = s.GetSnapshot();
    CHECK(b.by_name.size() == 3 && b.by_name[0].name == "c" && b.by_name[1].name == "a" && b.by_name[2].name == "b");
}

void TestDisableClearsResetKeeps()
{
    MessageStats s("t");
    s.Enable();
    s.Add(1, 1);
    s.Add("x", 1);
    s.Reset();
    CHECK(s.enabled()); // Reset 不改开关
    auto snap = s.GetSnapshot();
    CHECK(snap.by_id.empty() && snap.by_name.empty());
    s.Add(1, 2);
    CHECK(s.GetSnapshot().by_id.size() == 1);

    s.Disable();
    CHECK(!s.enabled());
    CHECK(s.GetSnapshot().by_id.empty());
    s.Add(1, 3); // 关着，不记
    s.Enable();
    CHECK(s.GetSnapshot().by_id.empty()); // 重新打开是从零开始
    s.Add(1, 4);
    CHECK(Same(s.GetSnapshot().by_id.at(0).total, {1, 4}));
}

// max_keys：两套键各自封顶；封顶之后新键进 dropped，旧键照常累加；总量守恒。
void TestMaxKeys()
{
    MessageStats s("t", {.interval_ms = 1000, .max_keys = 3});
    s.Enable();
    std::uint64_t sent_count = 0, sent_bytes = 0;
    auto add_id = [&](std::uint32_t id, std::size_t b)
    {
        s.Add(id, b);
        ++sent_count, sent_bytes += b;
    };
    auto add_name = [&](const char* n, std::size_t b)
    {
        s.Add(n, b);
        ++sent_count, sent_bytes += b;
    };
    for (std::uint32_t id = 1; id <= 5; ++id)
        add_id(id, id * 10); // 1 2 3 记下，4 5 丢
    add_id(2, 7);            // 已记下的键封顶后仍然累加
    add_id(4, 100);          // 被丢的键再来还是丢
    add_name("a", 1);
    add_name("b", 1);
    add_name("c", 1);
    add_name("d", 1); // 名字有自己的 3 个名额，不受 id 影响
    add_name("a", 1);

    const auto snap = s.GetSnapshot();
    CHECK(snap.by_id.size() == 3 && snap.by_name.size() == 3);
    std::uint64_t c = snap.dropped.count, b = snap.dropped.bytes;
    for (const auto& e : snap.by_id)
        c += e.total.count, b += e.total.bytes;
    for (const auto& e : snap.by_name)
        c += e.total.count, b += e.total.bytes;
    CHECK(c == sent_count && b == sent_bytes);
    CHECK(snap.dropped.count == 4 && snap.dropped.bytes == 40 + 50 + 100 + 1); // id 4、5、4，名字 d
    for (const auto& e : snap.by_id)
        if (e.id == 2)
            CHECK(Same(e.total, {2, 27}));

    s.Reset(); // 清空后名额也回来了
    s.Add(99, 1);
    CHECK(s.GetSnapshot().by_id.size() == 1 && s.GetSnapshot().dropped.count == 0);

    MessageStats zero("t", {.max_keys = 0}); // 0 名额：全部进 dropped，不崩
    zero.Enable();
    zero.Add(1, 5);
    zero.Add("x", 6);
    CHECK(zero.GetSnapshot().by_id.empty() && Same(zero.GetSnapshot().dropped, {2, 11}));
}

void TestFormat()
{
    MessageStats s("fmt", {.max_keys = 3});
    s.Enable();
    CHECK(s.Format() == "msgstats[fmt]"); // 没数据：只有标题
    s.Add(7, 10);
    s.Add(7, 10);
    s.Add(7, 10);
    s.Add(3, 5);
    s.Add("Foo", 4);
    s.Add("Foo", 4);
    CHECK(s.Format() ==
          "msgstats[fmt]\n"
          "  by id (2 keys, 4 msgs, 35 bytes):\n"
          "    7  3 msgs  30 bytes\n"
          "    3  1 msgs  5 bytes\n"
          "  by name (1 keys, 2 msgs, 8 bytes):\n"
          "    Foo  2 msgs  8 bytes");
    CHECK(Contains(s.Format(0, 250), "msgstats[fmt] window=250ms\n"));
    CHECK(Contains(s.Format(0, 0), "window=0ms")); // 0 也是有效的窗口长度

    // top_n 折叠：只列前 1 项，其余折成一行
    const std::string top1 = s.Format(1);
    CHECK(Contains(top1, "    7  3 msgs  30 bytes\n    ... 1 more"));
    CHECK(!Contains(top1, "    3  1 msgs"));
    CHECK(!Contains(top1, "... 0 more") && !Contains(s.Format(2), "more")); // 刚好列完不出折叠行

    // dropped 行
    s.Add(100, 1);
    s.Add(101, 2); // id 名额 3 个：7 3 100 占满，101 丢
    const std::string f = s.Format();
    CHECK(Contains(f, "\n  dropped (max_keys=3): 1 msgs  2 bytes"));
}

// Tick：窗口从第一次 Tick 起算；满 interval 才报（>=）；报完清零；没数据的窗口也算过去。
void TestTickWindow()
{
    std::vector<std::string> reports;
    const auto sink = [&](std::string_view r) { reports.emplace_back(r); };
    MessageStats s("t", {.interval_ms = 1000, .max_keys = 16, .report_top_n = 2});

    CHECK(!s.Tick(0, sink)); // 关着
    s.Enable();
    s.Add(1, 10);
    CHECK(!s.Tick(5000, sink)); // 第一次 Tick：只起算
    CHECK(!s.Tick(5999, sink)); // 差 1ms
    CHECK(reports.empty());
    CHECK(s.Tick(6000, sink)); // 恰好满（>=）
    CHECK(reports.size() == 1);
    CHECK(Contains(reports[0], "msgstats[t] window=1000ms") && Contains(reports[0], "    1  1 msgs  10 bytes"));
    CHECK(s.GetSnapshot().by_id.empty()); // 已清零

    // 没数据的窗口：不报，但窗口过去了，下一窗口从这一刻起算
    CHECK(!s.Tick(7000, sink));
    s.Add(2, 5);
    CHECK(!s.Tick(7999, sink));
    CHECK(s.Tick(8000, sink));
    CHECK(reports.size() == 2 && Contains(reports[1], "window=1000ms"));

    // 长时间没 Tick：报告里的窗口长度是实际过去的时间
    s.Add(3, 1);
    CHECK(s.Tick(20000, sink));
    CHECK(Contains(reports[2], "window=12000ms"));

    // report_top_n = 2：三项只列两项
    s.Add(1, 1);
    s.Add(1, 1);
    s.Add(2, 1);
    s.Add(3, 1);
    CHECK(s.Tick(21000, sink));
    CHECK(Contains(reports[3], "... 1 more"));

    // 回调在清零之后才调：回调里 Add 的数据留到下一窗口，不会被冲掉
    reports.clear();
    s.Add(4, 1);
    CHECK(s.Tick(22000, [&](std::string_view) { s.Add(9, 7); }));
    const auto snap = s.GetSnapshot();
    CHECK(snap.by_id.size() == 1 && snap.by_id[0].id == 9 && Same(snap.by_id[0].total, {1, 7}));
}

void TestTickEdgeCases()
{
    std::vector<std::string> reports;
    const auto sink = [&](std::string_view r) { reports.emplace_back(r); };

    // Disable 之后再 Enable：窗口重新起算，哪怕离上次 Tick 很久
    MessageStats s("t", {.interval_ms = 1000});
    s.Enable();
    s.Add(1, 1);
    CHECK(!s.Tick(0, sink));
    s.Disable();
    s.Enable();
    s.Add(1, 1);
    CHECK(!s.Tick(1'000'000, sink)); // 第一次 Tick：只起算，不报
    CHECK(s.Tick(1'001'000, sink));

    // interval_ms <= 0：每次 Tick（有数据时）都报
    for (const std::int64_t interval : {0LL, -5LL})
    {
        reports.clear();
        MessageStats e("t", {.interval_ms = interval});
        e.Enable();
        e.Add(1, 1);
        CHECK(!e.Tick(10, sink)); // 起算
        CHECK(e.Tick(10, sink));  // 同一毫秒也报
        e.Add(1, 1);
        CHECK(e.Tick(10, sink));
        CHECK(!e.Tick(10, sink)); // 没数据
        CHECK(reports.size() == 2);
        e.Add(1, 1);
        CHECK(e.Tick(5, sink)); // 时钟回退（不该发生，但约定的是"每次 Tick 都报"）也照报
        CHECK(reports.size() == 3);
    }

    // 只有名字（没有 id）的数据也算"有数据"
    reports.clear();
    MessageStats n("t", {.interval_ms = 1000});
    n.Enable();
    n.Add("OnlyName", 5);
    CHECK(!n.Tick(0, sink));
    CHECK(n.Tick(1000, sink) && Contains(reports.at(0), "OnlyName  1 msgs  5 bytes"));

    // dropped 也算"有数据"
    reports.clear();
    MessageStats d("t", {.interval_ms = 1000, .max_keys = 0});
    d.Enable();
    d.Add(1, 5);
    CHECK(!d.Tick(0, sink));
    CHECK(d.Tick(1000, sink) && Contains(reports.at(0), "dropped (max_keys=0): 1 msgs  5 bytes"));
}

void TestTickLogsByDefault()
{
    MessageStats s("logtest", {.interval_ms = 1000});
    s.Enable();
    s.Add(1001, 64);
    s.Add("PlayerMove", 8);
    CHECK(!s.Tick(0));
    bool fired = false;
    const std::string out = CaptureStderr([&] { fired = s.Tick(1000); });
    CHECK(fired); // 断言放在捕获之外：失败信息才不会被一起吞掉
    CHECK(Contains(out, "msgstats[logtest] window=1000ms"));
    CHECK(Contains(out, "1001  1 msgs  64 bytes") && Contains(out, "PlayerMove  1 msgs  8 bytes"));
    CHECK(Contains(out, "[info]"));
    fired = true;
    const std::string quiet = CaptureStderr([&] { fired = s.Tick(1500); });
    CHECK(!fired && quiet.empty());
}

// 随机操作序列：与 std::map 参考实现逐步对照（含 Reset / Disable / Enable / max_keys 封顶）。
void TestAgainstReferenceModel()
{
    std::mt19937_64 rng(20261030);
    for (int round = 0; round < 300; ++round)
    {
        const std::size_t cap = rng() % 9; // 0 .. 8
        MessageStats s("m", {.interval_ms = 1000, .max_keys = cap});
        s.Enable();
        bool enabled = true;
        std::map<std::uint32_t, Counter> ref_id;
        std::map<std::string, Counter> ref_name;
        Counter ref_drop;

        const auto bump = [](Counter& c, std::size_t b) { ++c.count, c.bytes += b; };
        for (int step = 0; step < 400; ++step)
        {
            const auto op = rng() % 24;
            const std::size_t bytes = rng() % 200;
            if (op < 10)
            {
                const auto id = static_cast<std::uint32_t>(rng() % 14);
                s.Add(id, bytes);
                if (enabled)
                {
                    auto it = ref_id.find(id);
                    if (it != ref_id.end())
                        bump(it->second, bytes);
                    else if (ref_id.size() < cap)
                        bump(ref_id[id], bytes);
                    else
                        bump(ref_drop, bytes);
                }
            }
            else if (op < 20)
            {
                const std::string name = "n" + std::to_string(rng() % 14);
                s.Add(name, bytes);
                if (enabled)
                {
                    auto it = ref_name.find(name);
                    if (it != ref_name.end())
                        bump(it->second, bytes);
                    else if (ref_name.size() < cap)
                        bump(ref_name[name], bytes);
                    else
                        bump(ref_drop, bytes);
                }
            }
            else if (op < 22)
            {
                s.Reset();
                ref_id.clear(), ref_name.clear(), ref_drop = {};
            }
            else
            {
                if (enabled)
                    s.Disable();
                else
                    s.Enable();
                enabled = !enabled;
                if (!enabled)
                    ref_id.clear(), ref_name.clear(), ref_drop = {};
            }

            if (step % 5 != 0)
                continue;
            const auto snap = s.GetSnapshot();
            CHECK(snap.by_id.size() == ref_id.size() && snap.by_name.size() == ref_name.size());
            CHECK(Same(snap.dropped, ref_drop));
            for (const auto& e : snap.by_id)
                CHECK(ref_id.count(e.id) && Same(ref_id[e.id], e.total));
            for (const auto& e : snap.by_name)
                CHECK(ref_name.count(e.name) && Same(ref_name[e.name], e.total));
            for (std::size_t i = 1; i < snap.by_id.size(); ++i)
            {
                const auto &a = snap.by_id[i - 1], &b = snap.by_id[i];
                CHECK(std::tie(b.total.count, b.total.bytes, a.id) < std::tie(a.total.count, a.total.bytes, b.id));
            }
            for (std::size_t i = 1; i < snap.by_name.size(); ++i)
            {
                const auto &a = snap.by_name[i - 1], &b = snap.by_name[i];
                CHECK(std::tie(b.total.count, b.total.bytes, a.name) < std::tie(a.total.count, a.total.bytes, b.name));
            }
        }
    }
}

} // namespace

int main()
{
    TestDisabledByDefault();
    TestCountsAndBytes();
    TestSnapshotOrder();
    TestDisableClearsResetKeeps();
    TestMaxKeys();
    TestFormat();
    TestTickWindow();
    TestTickEdgeCases();
    TestTickLogsByDefault();
    TestAgainstReferenceModel();
    std::puts("message_stats_test: ok");
    return 0;
}
