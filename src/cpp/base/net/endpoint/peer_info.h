// 对端信息：给 AcceptFilter 与 OnConnected 用。设计见 docs/网络库设计.md §4.4。

#pragma once

#include <cstdint>
#include <optional>

#include "endpoint.h"

namespace breeze
{

// SO_PEERCRED 取到的对端进程身份，只有 UDS 有。
struct PeerCredentials
{
    std::uint32_t uid = 0;
    std::uint32_t gid = 0;
    std::int32_t pid = 0;
};

struct PeerInfo
{
    // 对端地址。UDS 的客户端通常没有绑定路径（未命名），此时为空，要识别对端请看 credentials。
    std::optional<Endpoint> endpoint;
    std::optional<PeerCredentials> credentials;
};

} // namespace breeze
