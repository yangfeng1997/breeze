// 单 loop 的服务端：监听一个 endpoint，把接受的连接装配成 Connection 并交给同一个 EventLoop 持有。
//
// P2a 的垂直切片：EventLoopGroup / NetServer 出现之前，用它在一个 loop 上跑通 TCP 与 UDS（同一份代码）。
// 将来 NetServer 在此基础上把 accept 到的连接分发到 group 里别的 loop。
// 接受前过滤（AcceptFilter）、水位、idle 等选项也是 NetServer 的事，这里不做。

#pragma once

#include <memory>

#include "acceptor.h"
#include "connection_handler.h"
#include "event_loop.h"
#include "frame_codec.h"

namespace breeze
{

class LoopServer
{
public:
    // handler 与 factory 只持引用，调用方保证它们比 LoopServer 与它的连接活得久。
    LoopServer(EventLoop& loop,
               Endpoint endpoint,
               ConnectionHandler& handler,
               const FrameCodecFactory& factory,
               ConnectionOptions options = {});

    // 在 loop 线程上调用（loop 开始运行之前的装配阶段不限）。返回 errno，失败时已回滚。
    [[nodiscard]] int Start() { return acceptor_.Listen(); }
    void Stop() { acceptor_.Stop(); }

    // 实际绑定的地址：端口填 0 时可以从这里取到系统分配的端口。
    const Endpoint& endpoint() const noexcept { return acceptor_.endpoint(); }
    const Acceptor& acceptor() const noexcept { return acceptor_; }

private:
    void OnAccept(Fd fd, std::optional<Endpoint> peer);

    EventLoop& loop_;
    ConnectionHandler& handler_;
    const FrameCodecFactory& factory_;
    ConnectionOptions options_;
    Acceptor acceptor_; // 声明在最后：先于上面的成员销毁，销毁时 Stop 仍可用
};

} // namespace breeze
