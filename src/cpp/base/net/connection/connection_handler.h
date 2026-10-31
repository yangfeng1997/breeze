// 连接事件的回调接口，全部在连接所属 EventLoop 的 IO 线程上执行。设计见 docs/网络库设计.md §4.4。
//
// 回调必须非阻塞且 noexcept：IO 线程服务着成千上万条连接，阻塞它等于全体停摆；
// 回调里抛异常即 std::terminate（库里不 catch）。派生类必须同样声明 noexcept。

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace breeze
{

class Connection;

enum class CloseReason : std::uint8_t
{
    PeerClosed,    // 对端关闭（读到 EOF、HUP）
    LocalClose,    // 本端调用了 Close()
    IdleTimeout,   // idle 超时
    ProtocolError, // 解码失败 / 帧超长
    SlowConsumer,  // 发送积压超过 HighWaterMark
    IoError,       // 读写出错，errno 见 OnClosed 的 sys_errno
};
inline constexpr std::size_t kCloseReasonCount = static_cast<std::size_t>(CloseReason::IoError) + 1;

// 日志用。
constexpr const char* ToString(CloseReason r) noexcept
{
    switch (r)
    {
        case CloseReason::PeerClosed:
            return "PeerClosed";
        case CloseReason::LocalClose:
            return "LocalClose";
        case CloseReason::IdleTimeout:
            return "IdleTimeout";
        case CloseReason::ProtocolError:
            return "ProtocolError";
        case CloseReason::SlowConsumer:
            return "SlowConsumer";
        case CloseReason::IoError:
            return "IoError";
    }
    return "?";
}

class ConnectionHandler
{
public:
    virtual ~ConnectionHandler() = default;

    // 连接建立。Connection 只能在 IO 线程使用。
    virtual void OnConnected(Connection&) noexcept {}

    // payload 是读缓冲上的视图，只在回调内有效，要保留就自己拷贝；这是唯一必须实现的回调。
    virtual void OnMessage(Connection&, std::span<const std::byte> payload) noexcept = 0;

    // 每条已回调过 OnConnected 的连接恰触发一次，且在其最后一个 OnMessage 之后；此后不再有该连接的任何回调。
    // 触发时连接已经关闭（Send 返回 Closed），但对象仍然有效，回调返回后才销毁。
    virtual void OnClosed(Connection&, CloseReason, int /*sys_errno*/) noexcept {}
};

} // namespace breeze
