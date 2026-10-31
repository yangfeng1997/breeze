// RUDP 端点：UDP 的 IP 字面量加端口。是 Endpoint 的类型化外壳：将来的 RudpServer / RudpClient 只收它，
// 传不进 TCP 与 UDS 的端点（它们也收不了它）。校验规则同 Endpoint::Udp（只接 IP 字面量、不做 DNS、IP 规范化后保存）。
// 配置里写 udp://ip:port。

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "endpoint.h"

namespace breeze
{

class RudpEndpoint
{
public:
    static std::optional<RudpEndpoint> Make(std::string_view ip, std::uint16_t port);
    // udp://1.2.3.4:80 | udp://[::1]:80；其它写法（包括 tcp:// 与 unix://）返回 nullopt。
    static std::optional<RudpEndpoint> Parse(std::string_view text);
    // 从通用 Endpoint 转换；不是 UDP 的返回 nullopt。
    static std::optional<RudpEndpoint> From(const Endpoint& ep);

    const std::string& ip() const noexcept { return ep_.address(); }
    // 0 表示由系统分配（bind 用）。
    std::uint16_t port() const noexcept { return ep_.port(); }
    bool IsIPv6() const noexcept { return ep_.IsIPv6(); }
    std::string ToString() const { return ep_.ToString(); }

    // 通用形式：PeerInfo、日志、NetServer / NetClient 接口都用它。
    const Endpoint& endpoint() const noexcept { return ep_; }

    bool operator==(const RudpEndpoint&) const = default;

private:
    explicit RudpEndpoint(Endpoint ep)
        : ep_(std::move(ep))
    {
    }

    Endpoint ep_;
};

} // namespace breeze
