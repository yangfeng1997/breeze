// TCP 专有的连接选项。UDS 没有这些概念，所以不放进每种传输都用的 ConnectionOptions：
// 配在 UDS 上没有意义的字段，在 UDS 的选项类型里就根本不存在（以前是配了被静默忽略）。

#pragma once

#include <chrono>

namespace breeze
{

struct TcpOptions
{
    // TCP keepalive。只开 SO_KEEPALIVE 而不设参数时内核默认 7200s，等于没开，所以三个参数一起设；
    // 默认约 90s 发现对端消失。keepalive_idle 为 0 则不开 keepalive。
    std::chrono::seconds keepalive_idle{60};
    std::chrono::seconds keepalive_interval{10};
    int keepalive_count = 3;
    // TCP_USER_TIMEOUT：已发出的数据多久得不到确认就断开；0 = 不设。
    std::chrono::milliseconds tcp_user_timeout{0};
};

} // namespace breeze
