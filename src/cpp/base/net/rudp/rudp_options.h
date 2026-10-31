// rudp 的传输参数，服务端与客户端共用。设计见 docs/网络库设计.md §4.14。
//
// 两端的 mtu / 窗口 / 间隔各管各的，不要求相同；唯一要两端兼容的是 max_message_size：
// 收到比自己上限大的消息，会话以 ProtocolError 关闭。
// 默认值是游戏常用的"快速模式"（nodelay + 10ms 间隔 + 快速重传 + 关拥塞控制），拿带宽换延迟；
// 对公网大流量场景要把 no_congestion_control 关掉。

#pragma once

#include <cstddef>
#include <cstdint>

#include "rudp_protocol.h"

namespace breeze
{

struct RUdpOptions
{
    // 整个 UDP 载荷的上限（含 1 字节包头与 KCP 24 字节头）。1400 在以太网 + 隧道下一般不会被分片；
    // 范围 [rudp::kMinMtu, rudp::kMaxMtu]。
    std::uint32_t mtu = 1400;

    // 窗口，单位是段（一段 ≈ mtu - 25 字节）。recv_window 至少 128（KCP 的下限，也要容得下一条最大的消息）。
    // 暂停读（PauseRead）时靠接收窗口填满让对端慢下来；send_window 限界已发出、等确认的数据量，
    // ConnectionOptions::high_water_mark 只管还没进发送窗口的排队部分（见 RUdpConnection）。
    std::uint32_t send_window = 256;
    std::uint32_t recv_window = 256;

    // KCP 内部时钟的间隔，同时是定时 Update 的周期。越小延迟越低、空转越多；范围 [10, 100]。
    std::uint32_t interval_ms = 10;

    bool nodelay = true;
    int fast_resend = 2; // 被跳过几次就不等超时直接重传，0 = 关
    bool no_congestion_control = true;

    // 同一个段重传（含首次）这么多次仍没被确认，判链路已死，会话以 IoError（ETIMEDOUT）关闭。≥ 2。
    std::uint32_t dead_link = 20;

    // 一条消息的最大字节数（发送与接收都按它）。不能超过 rudp::MaxMessageForMtu(mtu)（127 段）。
    std::size_t max_message_size = 64 * 1024;

    // Send 之后立刻 flush，而不是等下一次定时 Update。延迟最多少一个 interval_ms，代价是每次 Send 一个数据报
    // （不再凑满一个 mtu 再发）；默认关。
    bool flush_on_send = false;

    // 套接字缓冲（SO_RCVBUF / SO_SNDBUF）的请求值，内核会按 net.core.rmem_max / wmem_max 截断；0 = 系统默认。
    // UDP 没有流控，缓冲小了突发时内核直接丢包。
    std::uint32_t socket_recv_buffer = 1u << 20;
    std::uint32_t socket_send_buffer = 1u << 20;

    bool Valid() const noexcept
    {
        return mtu >= rudp::kMinMtu && mtu <= rudp::kMaxMtu && send_window >= 1 && send_window <= 65535 &&
               recv_window >= 128 && recv_window <= 65535 && interval_ms >= 10 && interval_ms <= 100 &&
               fast_resend >= 0 && dead_link >= 2 && max_message_size >= 1 &&
               max_message_size <= rudp::MaxMessageForMtu(mtu);
    }
};

} // namespace breeze
