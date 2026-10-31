// 网络层统计的快照。设计见 docs/网络库设计.md §4.9。
//
// 每个 EventLoop 一份 relaxed 原子计数，EventLoopGroup::GetStats() 累加成这个普通结构体；
// 各计数之间不是同一时刻的一致快照（读的时候 IO 线程还在跑），只用于监控与诊断。

#pragma once

#include <array>
#include <chrono>
#include <cstdint>

#include "connection_handler.h"

namespace breeze
{

struct NetStats
{
    // ---- 当前值 ----
    std::uint64_t connections = 0;        // 已预留 + 已建立的连接数
    std::uint64_t paused_connections = 0; // 正被 PauseRead 暂停的连接数

    // ---- 累计 ----
    std::uint64_t accepted = 0;  // 服务端接受并建立（回调过 OnConnected）
    std::uint64_t connected = 0; // 客户端主动连上（回调过 OnConnected）
    // 回调过 OnClosed 的连接，按原因；下标是 static_cast<size_t>(CloseReason)
    std::array<std::uint64_t, kCloseReasonCount> closed{};

    // accept 之后被立即关闭的连接，不产生任何回调：
    std::uint64_t rejected_filter = 0;       // AcceptFilter 拒绝
    std::uint64_t rejected_full = 0;         // 所有 loop 的连接表都满
    std::uint64_t rejected_fd_exhausted = 0; // accept 遇 EMFILE / ENFILE
    std::uint64_t rejected_resource = 0;     // 新连接登记 epoll 失败（ENOSPC / ENOMEM）
    // rudp 的握手与数据报（字节流传输恒为 0）：
    std::uint64_t rejected_ticket = 0;   // Hello 被拒：票据不存在 / 已过期 / 已用过 / token 不对（也计入不了 accepted）
    std::uint64_t datagrams_dropped = 0; // 被丢弃的数据报：格式不对、来源没有会话、KCP 不认

    std::uint64_t bytes_in = 0;     // read 到的字节
    std::uint64_t bytes_out = 0;    // 实际写进 socket 的字节（含帧头）
    std::uint64_t messages_in = 0;  // 交给 OnMessage 的帧数
    std::uint64_t messages_out = 0; // Send 接受的帧数（Ok）

    // Send 返回 Closed 的次数，按原因：
    std::uint64_t send_closed_stale = 0;         // 句柄过期，或连接已关闭 / 正在关闭
    std::uint64_t send_closed_slow_consumer = 0; // 本次发送触发水位
    std::uint64_t send_closed_oversize = 0;      // payload 超过 codec 上限
    std::uint64_t send_closed_io_error = 0;      // 直写时 sendmsg 出错

    // 各 loop 里单轮（分发事件 + 任务 + 定时器 + 回收，不含 epoll_wait 睡眠）的最长耗时，取最大值。
    std::chrono::nanoseconds max_loop_lag{0};
};

} // namespace breeze
