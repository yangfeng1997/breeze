// 每连接的选项，建立时定下。NetServer / NetClient 的选项里有对应字段，由它们传进来。
// 库内这类策略值默认保持中立（idle 默认关闭）；生产默认值由 framework/net 的 net.proto 给出。

#pragma once

#include <chrono>
#include <cstddef>

namespace breeze
{

struct ConnectionOptions
{
    // 发送积压（排队 + 在途的 payload 字节）的软上限：超过它，本次 Send 返回 Closed，连接以 SlowConsumer 关闭。
    // 这是库内唯一的内建策略，作为内存安全兜底；检查与投递不是原子的，并发 Send 可能略超。
    // 与先例不同：brpc 超限返回 EOVERCROWDED 但保留连接，muduo / Netty 只通知应用；这里没有应用层策略钩子，
    // 关闭是写缓冲有上界的唯一手段。
    // 必须是第一个字段：测试与调用方用 ConnectionOptions{水位} 的聚合初始化。
    std::size_t high_water_mark = 256 * 1024;

    // 空闲超时：连续这么久没收到任何字节就以 IdleTimeout 关闭，0 = 关闭。"活跃"= 收到任何字节，是库唯一的定义；
    // 更严格的判定（必须收到合法心跳、按完整消息时间关闭滴灌式慢速连接）由应用层做。
    // 精度 ≤ 1s（每 100ms 扫 1/10 的槽）。被 PauseRead 暂停的连接不计空闲。
    std::chrono::milliseconds idle_timeout{0};

    // TCP keepalive（UDS 不适用）。只开 SO_KEEPALIVE 而不设参数时内核默认 7200s，等于没开，所以三个参数一起设；
    // 默认约 90s 发现对端消失。keepalive_idle 为 0 则不开 keepalive。
    std::chrono::seconds keepalive_idle{60};
    std::chrono::seconds keepalive_interval{10};
    int keepalive_count = 3;
    // TCP_USER_TIMEOUT：已发出的数据多久得不到确认就断开；0 = 不设。
    std::chrono::milliseconds tcp_user_timeout{0};
};

} // namespace breeze
