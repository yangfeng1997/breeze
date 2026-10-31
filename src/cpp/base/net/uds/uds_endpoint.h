// UDS 端点：路径或抽象名。是 Endpoint 的类型化外壳：UdsServer / UdsClient 只收它，传不进 TCP 的端点。
// 校验规则同 Endpoint::Unix / UnixAbstract（路径必须是绝对路径、都不能含 '\0'、长度 ≤107），拿到手的一定合法。

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "endpoint.h"

namespace breeze
{

class UdsEndpoint
{
public:
    static std::optional<UdsEndpoint> Path(std::string_view path);
    static std::optional<UdsEndpoint> Abstract(std::string_view name);
    // unix:///path | unix-abstract://name；其它写法（包括 tcp://）返回 nullopt。
    static std::optional<UdsEndpoint> Parse(std::string_view text);
    // 从通用 Endpoint 转换；不是 UDS 的返回 nullopt。
    static std::optional<UdsEndpoint> From(const Endpoint& ep);

    bool is_abstract() const noexcept { return ep_.kind() == Endpoint::Kind::UnixAbstract; }
    // 路径或抽象名。
    const std::string& address() const noexcept { return ep_.address(); }
    std::string ToString() const { return ep_.ToString(); }

    // 通用形式：PeerInfo、日志、StreamServer / StreamClient 基类都用它。
    const Endpoint& endpoint() const noexcept { return ep_; }

    bool operator==(const UdsEndpoint&) const = default;

private:
    explicit UdsEndpoint(Endpoint ep)
        : ep_(std::move(ep))
    {
    }

    Endpoint ep_;
};

} // namespace breeze
