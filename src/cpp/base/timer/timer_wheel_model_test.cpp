// TimerWheel 与一个"一眼看得出对不对"的参考实现逐步对照：
//   参考实现把所有定时器放进 std::map，按 (到期 tick, 创建序号) 排序，推进时从最小的开始弹出 ——
//   没有层级、没有降级、没有位图。 对随机的操作序列（添加 / 重复 / 取消 / 重置 / 推进，含 tick 精度
//   1、7、10，跨越四层轮甚至超出轮的延迟，回调里再 添加 / 取消 / 重置 /
//   取消自己）两边必须在每一步给出同样的结果：触发的 (标签, tick) 序列、返回值、size()、IsActive()。
// 再加：NextDeadlineMs 必须是下界（推进到它之前不会有任何触发）。无测试框架依赖：失败即 abort。

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <set>
#include <tuple>
#include <vector>

#include "base/timer/timer_wheel.h"

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

using Repeat = TimerWheel::Repeat;
using TimerId = TimerWheel::TimerId;

// ---------------- 参考实现 ----------------

class Model
{
public:
    using Callback = std::move_only_function<void()>;

    explicit Model(std::int64_t tick_ms)
        : tick_ms_(tick_ms >= 1 ? tick_ms : 1)
    {
    }

    std::size_t Advance(std::int64_t now_ms)
    {
        if (advancing_)
            return 0;
        if (!initialized_)
        {
            base_ms_ = now_ms;
            initialized_ = true;
            return 0;
        }
        if (now_ms < base_ms_)
            return 0;
        const std::int64_t target = (now_ms - base_ms_) / tick_ms_;
        advancing_ = true;
        target_ = target;
        std::size_t fired = 0;
        while (!order_.empty())
        {
            const auto [expire, seq, id] = *order_.begin();
            if (expire > target)
                break;
            order_.erase(order_.begin());
            cur_ = expire;
            T& t = timers_.at(id);
            t.state = 1; // 触发中
            if (t.interval == 0)
                --size_;
            t.cb();
            ++fired;
            // 注意：回调里可能增删 timers_，std::map 的节点引用不会失效，t 仍然有效
            if (t.interval > 0 && !t.stop)
            {
                std::int64_t next = cur_ + t.interval;
                if (t.repeat == Repeat::kSkipMissed && next <= target_)
                    next += t.interval * ((target_ - next) / t.interval + 1);
                t.expire = next;
                t.seq = next_seq_++;
                t.state = 0;
                order_.insert({t.expire, t.seq, id});
            }
            else
            {
                timers_.erase(id);
            }
        }
        if (target > cur_)
            cur_ = target;
        advancing_ = false;
        return fired;
    }

    TimerId AddTimer(std::int64_t delay_ms, Callback cb)
    {
        return Add(delay_ms, false, std::move(cb), Repeat::kSkipMissed);
    }
    TimerId AddRepeating(std::int64_t interval_ms, Callback cb, Repeat policy)
    {
        return Add(interval_ms, true, std::move(cb), policy);
    }

    bool Cancel(TimerId id)
    {
        auto it = timers_.find(id);
        if (it == timers_.end())
            return false;
        T& t = it->second;
        if (t.state == 0)
        {
            order_.erase({t.expire, t.seq, id});
            --size_;
            timers_.erase(it);
            return true;
        }
        if (t.interval > 0 && !t.stop)
        {
            t.stop = true;
            --size_;
            return true;
        }
        return false;
    }

    bool Reset(TimerId id, std::int64_t delay_ms)
    {
        auto it = timers_.find(id);
        if (it == timers_.end() || it->second.state != 0)
            return false;
        T& t = it->second;
        order_.erase({t.expire, t.seq, id});
        t.expire = cur_ + Ticks(delay_ms);
        t.seq = next_seq_++;
        order_.insert({t.expire, t.seq, id});
        return true;
    }

    bool IsActive(TimerId id) const
    {
        auto it = timers_.find(id);
        if (it == timers_.end())
            return false;
        const T& t = it->second;
        return t.state == 0 || (t.interval > 0 && !t.stop);
    }

    std::size_t size() const { return size_; }
    std::int64_t current_tick() const { return cur_; }

    // 参考实现里下一个到期的 tick；没有返回 -1
    std::int64_t EarliestExpire() const { return order_.empty() ? -1 : std::get<0>(*order_.begin()); }
    std::int64_t base_ms() const { return base_ms_; }
    bool initialized() const { return initialized_; }
    std::int64_t tick_ms() const { return tick_ms_; }

private:
    struct T
    {
        Callback cb;
        std::int64_t expire = 0;
        std::int64_t interval = 0;
        std::uint64_t seq = 0;
        Repeat repeat = Repeat::kSkipMissed;
        int state = 0; // 0 待触发，1 回调中
        bool stop = false;
    };

    // 与 TimerWheel::ToTicks 同一个约定，但用 128 位算，不会溢出
    std::int64_t Ticks(std::int64_t ms) const
    {
        if (ms <= 0)
            return 1;
        const __int128 q = (static_cast<__int128>(ms) + tick_ms_ - 1) / tick_ms_;
        const __int128 cap = static_cast<__int128>(1) << 60;
        return static_cast<std::int64_t>(q < cap ? q : cap);
    }

    TimerId Add(std::int64_t delay_ms, bool repeating, Callback cb, Repeat policy)
    {
        if (!cb)
            return 0;
        const TimerId id = ++next_id_;
        T& t = timers_[id];
        t.cb = std::move(cb);
        const std::int64_t ticks = Ticks(delay_ms);
        t.interval = repeating ? ticks : 0;
        t.repeat = policy;
        t.expire = cur_ + ticks;
        t.seq = next_seq_++;
        order_.insert({t.expire, t.seq, id});
        ++size_;
        return id;
    }

    std::int64_t tick_ms_;
    std::int64_t base_ms_ = 0;
    std::int64_t cur_ = 0;
    std::int64_t target_ = 0;
    bool initialized_ = false;
    bool advancing_ = false;
    std::uint64_t next_seq_ = 1;
    TimerId next_id_ = 0;
    std::size_t size_ = 0;
    std::map<TimerId, T> timers_;
    std::set<std::tuple<std::int64_t, std::uint64_t, TimerId>> order_;
};

// ---------------- 对照用例 ----------------

struct Fire
{
    int label;
    std::int64_t tick;
    bool operator==(const Fire&) const = default;
};

std::uint64_t Mix(std::uint64_t x)
{
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

// 一个"世界"：一个定时器实现（时间轮或参考实现）+ 它自己的标签表与触发记录。回调里的动作只由 (标签, 第几次触发) 决定，
// 与实现无关，两个世界在行为一致的前提下会做完全一样的事。
template <typename W>
struct World
{
    explicit World(std::int64_t tick_ms)
        : w(tick_ms)
    {
    }

    W w;
    std::vector<TimerId> ids; // 下标 = 标签
    std::vector<int> fires;   // 每个标签触发过几次
    std::vector<Fire> trace;
    bool allow_catch_up = false; // 为 false 时重复定时器一律 kSkipMissed
    bool made_catch_up = false;  // 出现过补触发的重复定时器：之后推进的步长要限制，否则一次大跳跃会触发天文数字次
    static constexpr std::size_t kMaxLabels = 4000; // 回调里再添加会滚雪球，封顶（两个世界一致地封顶）

    int NewLabel()
    {
        ids.push_back(0);
        fires.push_back(0);
        return static_cast<int>(ids.size()) - 1;
    }

    typename W::Callback Make(int label)
    {
        return [this, label]
        {
            const int k = fires[static_cast<std::size_t>(label)]++;
            trace.push_back({label, w.current_tick()});
            Act(label, k);
        };
    }

    std::int64_t DelayFor(std::uint64_t r) const
    {
        switch (r % 6)
        {
            case 0:
                return static_cast<std::int64_t>(r / 6 % 4); // 含 0
            case 1:
                return static_cast<std::int64_t>(r / 6 % 300); // 第 0 层
            case 2:
                return static_cast<std::int64_t>(r / 6 % 70000); // 到第 1 层
            case 3:
                return static_cast<std::int64_t>(r / 6 % 17000000); // 到第 2 层
            case 4:
                return static_cast<std::int64_t>(r / 6 % 4300000000ULL); // 到第 3 层
            default:
                return static_cast<std::int64_t>(r / 6 % 5000000000000ULL); // 超出四层
        }
    }

    TimerId AddOneShot(std::uint64_t r)
    {
        if (ids.size() >= kMaxLabels)
            return 0;
        const int label = NewLabel();
        ids[static_cast<std::size_t>(label)] = w.AddTimer(DelayFor(r), Make(label));
        CHECK(ids[static_cast<std::size_t>(label)] != 0);
        return ids[static_cast<std::size_t>(label)];
    }

    void AddRepeat(std::uint64_t r)
    {
        if (ids.size() >= kMaxLabels)
            return;
        const int label = NewLabel();
        const bool catch_up = allow_catch_up && (r >> 40) % 2 == 0;
        made_catch_up |= catch_up;
        // 补触发的重复定时器周期要够大，否则一次大跳跃会触发天文数字次
        const std::int64_t interval =
            catch_up ? 1000 + static_cast<std::int64_t>(r % 100000) : DelayFor(r % 4 == 0 ? r : r % 4000 + 1);
        ids[static_cast<std::size_t>(label)] =
            w.AddRepeating(interval, Make(label), catch_up ? Repeat::kCatchUp : Repeat::kSkipMissed);
        CHECK(ids[static_cast<std::size_t>(label)] != 0);
    }

    TimerId IdOf(std::uint64_t r) const
    {
        if (ids.empty())
            return 0;
        return ids[static_cast<std::size_t>(r % ids.size())];
    }

    void Act(int label, int k)
    {
        const std::uint64_t r = Mix(static_cast<std::uint64_t>(label) * 1000003ULL + static_cast<std::uint64_t>(k));
        switch (r % 10)
        {
            case 0:
            case 1:
                AddOneShot(r >> 8);
                break;
            case 2:
                AddRepeat(r >> 8);
                break;
            case 3:
                w.Cancel(IdOf(r >> 8));
                break;
            case 4:
                w.Reset(IdOf(r >> 8), DelayFor(r >> 16));
                break;
            case 5:
                w.Cancel(ids[static_cast<std::size_t>(label)]);
                break; // 取消自己
            case 6:
                w.Cancel(IdOf(static_cast<std::uint64_t>(label) + 1));
                break; // 取消（可能是同一 tick 里还没轮到的）下一个
            case 7:
                CHECK(!w.Reset(ids[static_cast<std::size_t>(label)], 5));
                break; // 回调里重置自己：不允许
            default:
                break;
        }
    }
};

template <typename A, typename B>
void CompareState(World<A>& a, World<B>& b)
{
    CHECK(a.trace == b.trace);
    CHECK(a.w.size() == b.w.size());
    CHECK(a.ids.size() == b.ids.size());
    // 抽查 IsActive（标签与句柄一一对应）
    for (std::size_t i = 0; i < a.ids.size(); i += 1 + a.ids.size() / 50)
        CHECK(a.w.IsActive(a.ids[i]) == b.w.IsActive(b.ids[i]));
}

void RunScenario(std::uint64_t seed, std::int64_t tick_ms, int steps)
{
    std::mt19937_64 rng(seed);
    World<TimerWheel> wheel(tick_ms);
    World<Model> model(tick_ms);
    wheel.allow_catch_up = model.allow_catch_up = true;

    std::int64_t now = static_cast<std::int64_t>(rng() % 1000000) - 500000; // 基准可以是负数
    CHECK(wheel.w.Advance(now) == 0 && model.w.Advance(now) == 0);

    for (int step = 0; step < steps; ++step)
    {
        const std::uint64_t r = rng();
        switch (r % 12)
        {
            case 0:
            case 1:
            case 2:
            case 3:
            {
                const std::uint64_t d = rng();
                wheel.AddOneShot(d);
                model.AddOneShot(d);
                break;
            }
            case 4:
            {
                const std::uint64_t d = rng();
                wheel.AddRepeat(d);
                model.AddRepeat(d);
                break;
            }
            case 5:
            {
                const std::uint64_t d = rng();
                CHECK(wheel.w.Cancel(wheel.IdOf(d)) == model.w.Cancel(model.IdOf(d)));
                break;
            }
            case 6:
            {
                const std::uint64_t d = rng(), e = rng();
                CHECK(wheel.w.Reset(wheel.IdOf(d), wheel.DelayFor(e)) ==
                      model.w.Reset(model.IdOf(d), model.DelayFor(e)));
                break;
            }
            default: // 推进：多数是小步，偶尔大跳（跨层、跨越整个四层轮）
            {
                std::int64_t step_ms;
                switch (rng() % 8)
                {
                    case 0:
                        step_ms = 0;
                        break;
                    case 1:
                        step_ms = 1;
                        break;
                    case 2:
                        step_ms = static_cast<std::int64_t>(rng() % 50);
                        break;
                    case 3:
                        step_ms = static_cast<std::int64_t>(rng() % 600);
                        break;
                    case 4:
                        step_ms = static_cast<std::int64_t>(rng() % 100000);
                        break;
                    case 5:
                        step_ms = static_cast<std::int64_t>(rng() % 20000000);
                        break;
                    case 6:
                        step_ms = -static_cast<std::int64_t>(rng() % 100);
                        break; // 时间回退：忽略
                    default:
                        step_ms = static_cast<std::int64_t>(rng() % 6000000000ULL);
                        break;
                }
                if (wheel.made_catch_up && step_ms > 3000000)
                    step_ms %= 3000000;
                now += step_ms;
                const std::size_t a = wheel.w.Advance(now);
                const std::size_t b = model.w.Advance(now);
                CHECK(a == b);
                break;
            }
        }
        CompareState(wheel, model);

        // NextDeadlineMs 是下界：参考实现里最早的到期时刻不会早于它；推进到它的前一毫秒不会触发任何东西
        const auto dl = wheel.w.NextDeadlineMs();
        const std::int64_t earliest = model.w.EarliestExpire();
        if (earliest < 0)
        {
            CHECK(!dl.has_value()); // 没有待触发的定时器
        }
        else
        {
            CHECK(dl.has_value());
            CHECK(*dl <= model.w.base_ms() + earliest * model.w.tick_ms());
            if (rng() % 4 == 0 && *dl - 1 > now && !(wheel.made_catch_up && *dl - 1 - now > 3000000))
            {
                now = *dl - 1;
                CHECK(wheel.w.Advance(now) == 0);
                CHECK(model.w.Advance(now) == 0);
            }
        }
    }
    CompareState(wheel, model);
}

// 每种 tick 精度跑多少个随机场景：默认够快（消毒器下也只要几十秒），要深挖时用环境变量放大，如
// BREEZE_TIMER_MODEL_SEEDS=2000。
void TestDifferential()
{
    long seeds = 40;
    if (const char* env = std::getenv("BREEZE_TIMER_MODEL_SEEDS"))
        seeds = std::max(1L, std::atol(env));
    for (const std::int64_t tick_ms : {1, 7, 10})
    {
        for (std::uint64_t seed = 1; seed <= static_cast<std::uint64_t>(seeds); ++seed)
            RunScenario(seed * 7919 + static_cast<std::uint64_t>(tick_ms), tick_ms, 400);
    }
}

// 只用一次性定时器、很多个、延迟横跨四层轮：每个必须在且仅在它的 tick 触发，顺序与参考实现相同。
void TestManyOneShots()
{
    for (const std::int64_t tick_ms : {1, 3})
    {
        World<TimerWheel> wheel(tick_ms);
        World<Model> model(tick_ms);
        std::mt19937_64 rng(99 + static_cast<std::uint64_t>(tick_ms));
        wheel.w.Advance(1000);
        model.w.Advance(1000);
        for (int i = 0; i < 30000; ++i)
        {
            const std::uint64_t d = rng();
            // 不让回调再生出新的：这里要测的是纯粹的到期时刻
            wheel.ids.push_back(wheel.w.AddTimer(wheel.DelayFor(d),
                                                 [&wheel, i] { wheel.trace.push_back({i, wheel.w.current_tick()}); }));
            model.ids.push_back(model.w.AddTimer(model.DelayFor(d),
                                                 [&model, i] { model.trace.push_back({i, model.w.current_tick()}); }));
        }
        std::int64_t now = 1000;
        while (wheel.w.size() > 0)
        {
            const auto dl = wheel.w.NextDeadlineMs();
            CHECK(dl.has_value());
            now = std::max(now, *dl);
            const std::size_t a = wheel.w.Advance(now);
            const std::size_t b = model.w.Advance(now);
            CHECK(a == b);
            CHECK(wheel.trace == model.trace);
        }
        CHECK(model.w.size() == 0);
        CHECK(wheel.trace.size() == 30000);
        CHECK(wheel.trace == model.trace);
    }
}

} // namespace

int main()
{
    TestDifferential();
    TestManyOneShots();
    std::puts("timer_wheel_model_test: ok");
    return 0;
}
