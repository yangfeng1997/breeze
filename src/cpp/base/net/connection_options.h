// 每连接的选项，建立时定下。NetServer / NetClient 的选项里有对应字段，由它们传进来。

#pragma once

#include <cstddef>

namespace breeze
{

struct ConnectionOptions
{
    // 发送积压（排队 + 在途的 payload 字节）的软上限：超过它，本次 Send 返回 Closed，连接以 SlowConsumer 关闭。
    // 这是库内唯一的内建策略，作为内存安全兜底；检查与投递不是原子的，并发 Send 可能略超。
    // 与先例不同：brpc 超限返回 EOVERCROWDED 但保留连接，muduo / Netty 只通知应用；这里没有应用层策略钩子，
    // 关闭是写缓冲有上界的唯一手段。
    std::size_t high_water_mark = 256 * 1024;
};

} // namespace breeze
