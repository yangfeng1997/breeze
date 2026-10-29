#pragma once

#include <cstdint>

namespace breeze
{

enum class SendResult : std::uint8_t
{
    Ok,     // 网络层已接收，不表示送达
    Closed, // 连接已关闭或正在关闭（句柄过期、状态非 Connected、本次发送触发慢消费者关闭），原因见 OnClosed
};

} // namespace breeze
