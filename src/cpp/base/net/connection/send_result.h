#pragma once

#include <cstdint>

namespace breeze
{

enum class SendResult : std::uint8_t
{
    Ok,     // 网络层已接收，不表示送达
    Closed, // 连接已关闭或正在关闭（句柄过期、状态非 Connected、本次发送触发慢消费者关闭），原因见 OnClosed
    // 下游暂时放不下（共享内存的 Tx 环满：Agent 没来得及搬或不在）：**本条没有发出，连接还开着**，可以稍后重试或自行丢弃。
    // 只有 Full 能保证"没发出"；返回 Ok 的消息在跨线程发送时仍可能随后因环满被丢（写环在 IO 线程上异步进行），那部分只进计数。
    Full,
};

} // namespace breeze
