// 每个 EventLoop 一份的计数。设计见 docs/网络库设计.md §4.9。
//
// 两类写法，不能混用在同一个计数上：
//   单写者（只有所属 IO 线程写）：Bump / Drop = load + store，不是 RMW。热路径每条消息都要记，lock xadd 太贵；
//     别的线程只读，relaxed 读到的是某个较新的值。
//   多写者（ConnectionHandle::Send 在任意线程发现 Closed）：fetch_add。只发生在关闭 / 出错路径，不热。

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

#include "base/net/loop/net_stats.h"

namespace breeze
{

struct alignas(64) LoopStats
{
    using Counter = std::atomic<std::uint64_t>;

    static void Bump(Counter& c, std::uint64_t n = 1) noexcept
    {
        c.store(c.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
    }
    // 配合 Bump 的 Gauge：无符号回绕相加，等价于减。调用方保证不减到负。
    static void Drop(Counter& c, std::uint64_t n = 1) noexcept
    {
        c.store(c.load(std::memory_order_relaxed) - n, std::memory_order_relaxed);
    }

    // ---- 单写者：所属 IO 线程 ----
    Counter accepted{0};
    Counter connected{0};
    Counter closed[kCloseReasonCount]{};
    Counter rejected_filter{0};
    Counter rejected_full{0};
    Counter rejected_fd_exhausted{0};
    Counter rejected_resource{0};
    Counter rejected_ticket{0};   // rudp：Hello 的票据不存在 / 过期 / 已用 / token 不对
    Counter datagrams_dropped{0}; // rudp：格式不对、来源未登记、KCP 不认的数据报
    Counter rudp_updates{0};      // rudp：定时器真正去推进 KCP（调了 Update）的次数；没到点的会话不计
    Counter bytes_in{0};
    Counter bytes_out{0};
    Counter messages_in{0};
    Counter messages_out{0};
    Counter paused{0}; // 当前暂停的连接数（Gauge）
    Counter max_lag_ns{0};

    // ---- 多写者：任意线程，只用 fetch_add ----
    Counter send_closed_stale{0};
    Counter send_closed_slow_consumer{0};
    Counter send_closed_oversize{0};
    Counter send_closed_io_error{0};

    static void Add(Counter& c) noexcept { c.fetch_add(1, std::memory_order_relaxed); }

    // 累加进 out（max_loop_lag 取最大值）。connections 由调用方按连接表另填。
    void AddTo(NetStats& out) const noexcept
    {
        const auto rd = [](const Counter& c) { return c.load(std::memory_order_relaxed); };
        out.paused_connections += rd(paused);
        out.accepted += rd(accepted);
        out.connected += rd(connected);
        for (std::size_t i = 0; i < kCloseReasonCount; ++i)
            out.closed[i] += rd(closed[i]);
        out.rejected_filter += rd(rejected_filter);
        out.rejected_full += rd(rejected_full);
        out.rejected_fd_exhausted += rd(rejected_fd_exhausted);
        out.rejected_resource += rd(rejected_resource);
        out.rejected_ticket += rd(rejected_ticket);
        out.datagrams_dropped += rd(datagrams_dropped);
        out.rudp_updates += rd(rudp_updates);
        out.bytes_in += rd(bytes_in);
        out.bytes_out += rd(bytes_out);
        out.messages_in += rd(messages_in);
        out.messages_out += rd(messages_out);
        out.send_closed_stale += rd(send_closed_stale);
        out.send_closed_slow_consumer += rd(send_closed_slow_consumer);
        out.send_closed_oversize += rd(send_closed_oversize);
        out.send_closed_io_error += rd(send_closed_io_error);
        const std::chrono::nanoseconds lag(static_cast<std::int64_t>(rd(max_lag_ns)));
        if (lag > out.max_loop_lag)
            out.max_loop_lag = lag;
    }
};

} // namespace breeze
