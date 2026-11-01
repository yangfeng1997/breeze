// 业务实现的回调接口，全部在 App 主循环线程上执行（经 MainLoopExecutor::Drain）。设计见 docs/网络库设计.md §5 / §6。
//
// 与 base/net 的 IConnectionHandler 的区别：线程不同，所以拿到的是可跨线程保存的 ConnectionHandle 与自己拥有的
// SharedBuffer， 不是只在 IO 线程有效的 IConnection& 与回调内有效的视图。同一条连接的事件全序：OnConnected → OnMessage*
// → OnClosed。

#pragma once

#include "base/buffer/shared_buffer.h"
#include "base/net/connection/connection_handle.h"
#include "base/net/connection/connection_handler_interface.h"
#include "base/net/endpoint/peer_info.h"

namespace breeze
{

class IMainLoopHandler
{
public:
    virtual ~IMainLoopHandler() = default;

    virtual void OnConnected(ConnectionHandle, const PeerInfo&) {}
    // 唯一必须实现的回调。msg 归业务所有，可以保存、跨线程传递。
    virtual void OnMessage(ConnectionHandle, SharedBuffer msg) = 0;
    // 每条回调过 OnConnected 的连接恰一次，且在它最后一个 OnMessage 之后。
    virtual void OnClosed(ConnectionHandle, CloseReason, int /*sys_errno*/) {}
};

} // namespace breeze
