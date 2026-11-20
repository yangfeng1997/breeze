// codec 回调连接的接口：有状态的协议（握手、控制帧、关闭握手）需要在 Decode 里主动写字节、通知握手完成、要求关闭。
// 由连接实现，经 IFrameCodec::Attach 交给 codec；无状态的 codec（LengthPrefixCodec）用不到它。设计见方案 §5.1。
//
// 只能在连接所属 IO 线程上调用：Decode / Attach / OnClosing 里。回调里可以重入（codec 在 Decode 里调 SendRaw、
// HandshakeDone、CloseAfterFlush 都是合法的）；连接已经关闭之后调用是空操作。
// 是纯接口，codec 的单元测试可以用假实现直接喂字节，不需要真连接。

#pragma once

#include <cstddef>
#include <memory>
#include <span>

#include "base/net/connection/connection_context.h"
#include "base/net/connection/connection_handler_interface.h"

namespace breeze
{

class ICodecHost
{
public:
    virtual ~ICodecHost() = default;

    // 立即写出原始字节，不经 EncodeHeader；与 IConnection::Send 保序（共用同一条发送队列）。
    // 例：WebSocket 的 Pong、握手应答、Close 帧。bytes 返回后即可改写 / 释放。
    virtual void SendRaw(std::span<const std::byte> bytes) = 0;

    // codec 的握手完成。所有握手来源（层、codec）都完成后才回调 OnConnected。
    // 幂等：重复调用无害（按来源清位，不是计数）。只有 CodecTraits::needs_handshake 为真的 codec 才需要调。
    virtual void HandshakeDone() = 0;

    // 写完已排队的字节再关闭：连接转入 Disconnecting（Send 从此返回 Closed），排空后 shutdown(SHUT_WR)，
    // 对端关闭或超时后收尾。OnClosed 带出这里给的 reason 与 code（例：对端发来 WebSocket Close 帧，
    // 报 PeerClosed 并带出 close code），不是通常的 LocalClose。对非 Connected 的连接是空操作。
    virtual void CloseAfterFlush(CloseReason reason, int code) = 0;

    // 给连接装上每连接用户数据（语义同 IConnection::SetContext）。例：WebSocket 升级回调的产物。
    virtual void SetContext(std::unique_ptr<ConnectionContext> ctx) = 0;
};

} // namespace breeze
