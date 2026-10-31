// 准入票据：控制面（login / gate）经可信通道（TCP + TLS 之类）发给客户端，客户端在 UDP 握手（Hello）里出示一次。
// 服务端只收登记过的、未过期的、第一次出示的票据：UDP 上随便一个包就能让服务端建会话的口子被堵上了。
// 设计与威胁模型见 docs/网络库设计.md §4.14。
//
// conv 是会话号（KCP 用它标识会话，非 0），token 是 16 字节随机数（密钥级别，别写进日志）。

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace breeze
{

struct RudpTicket
{
    static constexpr std::size_t kTokenSize = 16;

    std::uint32_t conv = 0;
    std::array<std::byte, kTokenSize> token{};

    bool valid() const noexcept { return conv != 0; }
    bool operator==(const RudpTicket&) const = default;
};

} // namespace breeze
