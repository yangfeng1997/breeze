// 准入票据表：一次性、过期、token 错不烧票据、容量、conv 冲突、并发签发与取用。
// 时间都由测试传入（TicketTable 的 now 参数），不睡觉。无测试框架依赖：失败即 abort。

#include "base/net/rudp/internal/ticket_table.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

using namespace breeze;
using namespace std::chrono_literals;
using Clock = TicketTable::Clock;

#define CHECK(...)                                                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(__VA_ARGS__))                                                                                            \
        {                                                                                                              \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #__VA_ARGS__);                       \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (0)

namespace
{

const Clock::time_point t0 = Clock::time_point{} + 1000s;

RUdpTicket MakeTicket(std::uint32_t conv, int fill)
{
    RUdpTicket t;
    t.conv = conv;
    t.token.fill(static_cast<std::byte>(fill));
    return t;
}

void TestIssueAndConsume()
{
    TicketTable tbl(100);
    const auto t = tbl.Issue(10s, t0);
    CHECK(t && t->valid());
    CHECK(tbl.size() == 1);
    CHECK(tbl.Consume(t->conv, t->token, t0 + 1s));
    CHECK(tbl.size() == 0);
}

// 签发出来的 token 是随机的（不是全零、两张不同）、conv 非 0 且互不相同。
void TestIssueIsRandom()
{
    TicketTable tbl(1000);
    std::set<std::uint32_t> convs;
    std::set<std::array<std::byte, 16>> tokens;
    for (int i = 0; i < 200; ++i)
    {
        const auto t = tbl.Issue(10s, t0);
        CHECK(t && t->conv != 0);
        CHECK(t->token != std::array<std::byte, 16>{});
        convs.insert(t->conv);
        tokens.insert(t->token);
    }
    CHECK(convs.size() == 200 && tokens.size() == 200);
    CHECK(tbl.size() == 200);
}

// 一次性：第二次出示同一张票据失败（重放）。
void TestSingleUse()
{
    TicketTable tbl(10);
    const RUdpTicket t = MakeTicket(5, 0xAB);
    CHECK(tbl.Add(t, 10s, t0));
    CHECK(tbl.Consume(t.conv, t.token, t0));
    CHECK(!tbl.Consume(t.conv, t.token, t0));
}

// token 错：失败，且不烧掉合法票据——知道 conv 的人不能靠试错作废别人的票据。
void TestWrongTokenDoesNotBurn()
{
    TicketTable tbl(10);
    const RUdpTicket t = MakeTicket(5, 0xAB);
    CHECK(tbl.Add(t, 10s, t0));
    RUdpTicket forged = t;
    forged.token[15] ^= std::byte{1}; // 只差最后一个字节
    CHECK(!tbl.Consume(forged.conv, forged.token, t0));
    forged = t;
    forged.token[0] ^= std::byte{0x80}; // 只差第一个字节
    CHECK(!tbl.Consume(forged.conv, forged.token, t0));
    CHECK(tbl.size() == 1);
    CHECK(tbl.Consume(t.conv, t.token, t0)); // 真的那张还能用
}

void TestUnknownConv()
{
    TicketTable tbl(10);
    CHECK(tbl.Add(MakeTicket(5, 1), 10s, t0));
    CHECK(!tbl.Consume(6, MakeTicket(6, 1).token, t0));
    CHECK(!tbl.Consume(0, MakeTicket(0, 1).token, t0));
}

// 过期：边界是半开的（正好到期那一刻已经无效）；过期的在取用时顺手删掉。
void TestExpiry()
{
    TicketTable tbl(10);
    const RUdpTicket t = MakeTicket(5, 1);
    CHECK(tbl.Add(t, 10s, t0));
    CHECK(!tbl.Consume(t.conv, t.token, t0 + 10s)); // 正好到期
    CHECK(tbl.size() == 0);                         // 顺手删了
    CHECK(tbl.Add(t, 10s, t0));
    CHECK(!tbl.Consume(t.conv, t.token, t0 + 11s));

    CHECK(tbl.Add(t, 10s, t0));
    CHECK(tbl.Consume(t.conv, t.token, t0 + 10s - 1ms)); // 差一毫秒还有效
}

void TestAddValidation()
{
    TicketTable tbl(10);
    CHECK(!tbl.Add(MakeTicket(0, 1), 10s, t0)); // conv 为 0
    CHECK(!tbl.Add(MakeTicket(1, 1), 0s, t0));  // ttl 为 0
    CHECK(!tbl.Add(MakeTicket(1, 1), -1s, t0));
    CHECK(!tbl.Issue(0s, t0));
    CHECK(!tbl.Issue(-5s, t0));
    CHECK(tbl.size() == 0);
}

// conv 已有未过期的：拒绝，不覆盖（覆盖会让先拿到票据的人莫名失败）；过期的可以被顶掉。
void TestConvConflict()
{
    TicketTable tbl(10);
    const RUdpTicket a = MakeTicket(5, 1);
    const RUdpTicket b = MakeTicket(5, 2);
    CHECK(tbl.Add(a, 10s, t0));
    CHECK(!tbl.Add(b, 10s, t0 + 1s));
    CHECK(tbl.Consume(a.conv, a.token, t0 + 2s)); // a 没被覆盖
    CHECK(tbl.Add(a, 10s, t0));
    CHECK(tbl.Add(b, 10s, t0 + 10s)); // a 已到期：b 顶掉它
    CHECK(!tbl.Consume(a.conv, a.token, t0 + 11s));
    CHECK(tbl.Consume(b.conv, b.token, t0 + 11s));
}

// conv 冲突的判定也是半开的：同 conv 的旧票据正好到期的那一刻，新的可以顶掉它。
// 这里不能让清扫抢先把旧的删了（那样根本走不到冲突判定）：第一次登记之后下一次清扫在 1 秒后。
void TestConvConflictBoundary()
{
    TicketTable tbl(10);
    const RUdpTicket a = MakeTicket(5, 1);
    const RUdpTicket b = MakeTicket(5, 2);
    CHECK(tbl.Add(a, 500ms, t0));
    CHECK(!tbl.Add(b, 10s, t0 + 499ms)); // 还差 1ms：冲突
    CHECK(tbl.Add(b, 10s, t0 + 500ms));  // 正好到期：可以顶掉
    CHECK(!tbl.Consume(a.conv, a.token, t0 + 501ms));
    CHECK(tbl.Consume(b.conv, b.token, t0 + 501ms));
}

// 容量：满了拒绝；清扫过期项之后有空位就能继续。
void TestCapacity()
{
    TicketTable tbl(3);
    CHECK(tbl.Add(MakeTicket(1, 1), 10s, t0));
    CHECK(tbl.Add(MakeTicket(2, 1), 10s, t0));
    CHECK(tbl.Add(MakeTicket(3, 1), 100s, t0));
    CHECK(!tbl.Add(MakeTicket(4, 1), 10s, t0 + 1s)); // 都没过期：满
    CHECK(!tbl.Issue(10s, t0 + 1s));
    CHECK(tbl.size() == 3);
    // 1 和 2 过期了：满的时候登记会先清扫，腾出位置
    CHECK(tbl.Add(MakeTicket(4, 1), 10s, t0 + 20s));
    CHECK(tbl.size() == 2); // 3 与 4
    CHECK(tbl.Consume(3, MakeTicket(3, 1).token, t0 + 20s));
    CHECK(tbl.Consume(4, MakeTicket(4, 1).token, t0 + 20s));
}

// 没满时过期项也会定期清扫（每秒一次），表不会无限长。
void TestPeriodicSweep()
{
    TicketTable tbl(1000);
    for (std::uint32_t i = 1; i <= 50; ++i)
        CHECK(tbl.Add(MakeTicket(i, 1), 1s, t0));
    CHECK(tbl.size() == 50);
    CHECK(tbl.Add(MakeTicket(100, 1), 1s, t0 + 5s)); // 离上次清扫已过 1 秒：顺手清掉前 50 张
    CHECK(tbl.size() == 1);
}

// 清扫的边界也是半开的：正好到期的那一刻已被清掉（不是"到期之后"才清）。
void TestSweepBoundary()
{
    TicketTable tbl(1000);
    CHECK(tbl.Add(MakeTicket(1, 1), 10s, t0));
    CHECK(tbl.Add(MakeTicket(2, 1), 10s + 1ms, t0)); // 晚 1ms 到期
    CHECK(tbl.size() == 2);
    CHECK(tbl.Add(MakeTicket(3, 1), 10s, t0 + 10s)); // 距上次清扫已过 1 秒：此刻 1 号恰好到期被清，2 号还在
    CHECK(tbl.size() == 2);
    CHECK(tbl.Consume(2, MakeTicket(2, 1).token, t0 + 10s));
}

// 多线程：控制面线程签发并交给"客户端"（共享队列），IO 线程并行取用；每张恰好成功一次，重放必败。
void TestConcurrent()
{
    TicketTable tbl(100000);
    constexpr int kPerIssuer = 2000;
    constexpr int kIssuers = 4;
    constexpr int kConsumers = 2;
    constexpr int kTotal = kPerIssuer * kIssuers;

    std::mutex mu;
    std::vector<RUdpTicket> handoff;
    std::atomic<int> issued{0};
    std::atomic<int> consumed{0};

    std::vector<std::thread> threads;
    for (int i = 0; i < kIssuers; ++i)
    {
        threads.emplace_back(
            [&]
            {
                for (int k = 0; k < kPerIssuer; ++k)
                {
                    const auto t = tbl.Issue(60s);
                    CHECK(t.has_value());
                    {
                        const std::lock_guard lk(mu);
                        handoff.push_back(*t);
                    }
                    issued.fetch_add(1);
                }
            });
    }
    for (int i = 0; i < kConsumers; ++i)
    {
        threads.emplace_back(
            [&]
            {
                for (;;)
                {
                    RUdpTicket t;
                    bool got = false;
                    {
                        const std::lock_guard lk(mu);
                        if (!handoff.empty())
                        {
                            t = handoff.back();
                            handoff.pop_back();
                            got = true;
                        }
                    }
                    if (!got)
                    {
                        if (issued.load() == kTotal)
                        {
                            const std::lock_guard lk(mu);
                            if (handoff.empty())
                                return;
                        }
                        std::this_thread::yield();
                        continue;
                    }
                    CHECK(tbl.Consume(t.conv, t.token));
                    CHECK(!tbl.Consume(t.conv, t.token)); // 重放
                    consumed.fetch_add(1);
                }
            });
    }
    for (auto& t : threads)
        t.join();
    CHECK(consumed.load() == kTotal);
    CHECK(tbl.size() == 0);
}

} // namespace

int main()
{
    TestIssueAndConsume();
    TestIssueIsRandom();
    TestSingleUse();
    TestWrongTokenDoesNotBurn();
    TestUnknownConv();
    TestExpiry();
    TestAddValidation();
    TestConvConflict();
    TestConvConflictBoundary();
    TestCapacity();
    TestPeriodicSweep();
    TestSweepBoundary();
    TestConcurrent();
    std::puts("ticket_table_test: ok");
    return 0;
}
