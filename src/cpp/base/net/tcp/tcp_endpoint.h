// TCP 端点：IP 字面量加端口。是 Endpoint 的类型化外壳：TcpServer / TcpClient 只收它，传不进 UDS 的端点。
// 校验规则同 Endpoint::Tcp（只接 IP 字面量、不做 DNS、IP 规范化后保存），拿到手的一定合法。

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "endpoint.h"

namespace breeze
{

class TcpEndpoint
{
public:
    static std::optional<TcpEndpoint> Make(std::string_view ip, std::uint16_t port);
    // tcp://1.2.3.4:80 | tcp://[::1]:80；其它写法（包括 unix://）返回 nullopt。
    static std::optional<TcpEndpoint> Parse(std::string_view text);
    // 从通用 Endpoint 转换；不是 TCP 的返回 nullopt。
    static std::optional<TcpEndpoint> From(const Endpoint& ep);

    const std::string& ip() const noexcept { return ep_.address(); }
    // 0 表示由系统分配（bind 用）。
    std::uint16_t port() const noexcept { return ep_.port(); }
    bool IsIPv6() const noexcept { return ep_.IsIPv6(); }
    std::string ToString() const { return ep_.ToString(); }

    // 通用形式：PeerInfo、日志、StreamServer / StreamClient 基类都用它。
    const Endpoint& endpoint() const noexcept { return ep_; }

    bool operator==(const TcpEndpoint&) const = default;

private:
    explicit TcpEndpoint(Endpoint ep)
        : ep_(std::move(ep))
    {
    }

    Endpoint ep_;
};

} // namespace breeze
