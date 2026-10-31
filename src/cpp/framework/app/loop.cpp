// 主循环，以及它驱动的能力广播。对照 src/go/core/app/loop.go。

#include "framework/app/app.h"

namespace breeze
{

// 平时零 CPU 阻塞在 cv 上，被 Notify / 周期 / stop_token 唤醒。
//
// Go 的 select 在多个就绪分支里随机选，这里固定「先 tick、后 wake」：持续积压的 Waker
// 饿不死 tick，行为可预测。反过来 tick 也饿不死 wake —— 一轮超时后 next 被推到未来
// （见下，等价 time.Ticker 丢 tick），wake 才有机会跑。
//
// stop_token 到达那一刻若 tick / wake 恰好就绪，会多跑一轮，发生在 Run 返回之前所以安全；
// 测试不能断言「stop 之后零次回调」。
void App::RunLoop(std::stop_token st)
{
    // 没有 ITickable 时不进 tick 分支，wait 里也不带截止时间，不为永远没人用的定时器空转
    const bool ticking = opts_.tickInterval > std::chrono::nanoseconds::zero() && !tickables_.empty();
    auto next = ticking ? Clock::now() + opts_.tickInterval : Clock::time_point{};

    while (!st.stop_requested())
    {
        if (ticking)
        {
            const auto now = Clock::now();
            if (now >= next)
            {
                // tickCount 只在这里自增：数的是周期 tick 跑过多少次，不是循环转了多少圈。
                // 两次 Tick 之间穿插多少事件都不影响连续性，模块按 tickCount % N 分频才有意义。
                ++tickCount_;
                TickAll(now, tickCount_);

                next += opts_.tickInterval;
                // 一轮超时：丢掉错过的 tick，不补课（tickCount 会落后于时间，要精确时间差用 now）
                if (next <= Clock::now())
                    next = Clock::now() + opts_.tickInterval;
                continue;
            }
        }

        if (wake_.exchange(false, std::memory_order_acq_rel))
        {
            // 还有积压 → 自己补 token 再转一圈：进来的那个 token 已被上面的 exchange 消费，
            // 生产者的 Notify 早发完了，不补的话剩下的积压要等下一次投递才动。
            // 不在这里 while 重试，是为了回到循环顶让 tick 有机会先跑。
            if (WakeAll())
                wake_.store(true, std::memory_order_release);
            continue;
        }

        // 谓词在 wakeMu_ 下检查，Notify 先过一遍 wakeMu_ 再 notify，所以不会丢唤醒。
        std::unique_lock lk(wakeMu_);
        const auto pending = [this] { return wake_.load(std::memory_order_acquire); };
        if (ticking)
        {
            wakeCv_.wait_until(lk, st, next, pending);
        }
        else
        {
            wakeCv_.wait(lk, st, pending);
        }
    }
}

void App::Notify() noexcept
{
    // 已有 token 直接合并，只有第一个生产者会碰锁。
    if (wake_.exchange(true, std::memory_order_acq_rel))
        return;
    {
        std::lock_guard lk(wakeMu_);
    } // 与消费者的谓词检查串行，防止其检查完、进入 wait 前漏掉 notify
    wakeCv_.notify_one();
}

// 单轮处理多少由模块自定（见 IWaker）。返回 true = 还有模块报积压。
// 顺序 = 拓扑序 = Init 顺序，与 TickAll 一致。
bool App::WakeAll()
{
    bool more = false;
    for (const auto& w : wakers_)
    {
        // 已知 more 也必须把剩下的都问一遍：每个模块有自己的队列
        if (w.cap->Wake())
            more = true;
    }
    return more;
}

// tick 取正向（与 Init 同向，数据流向下游）；反过来只会让依赖者的 tick 跑在被依赖者之前。
void App::TickAll(Clock::time_point now, std::uint64_t tickCount)
{
    for (const auto& t : tickables_)
        t.cap->Tick(now, tickCount);
}

} // namespace breeze
