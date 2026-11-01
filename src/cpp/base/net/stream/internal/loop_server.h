// 单 loop 的服务端：监听一个 endpoint，把接受的连接装配成 IConnection 并交给同一个 EventLoop 持有。
//
// 只有测试与 bench 在用（connection_handle_test、net_flow_test、echo_test、net_bench 的 breeze 直接回调基线）；
// 生产路径是 StreamServer：挂在 EventLoopGroup 上，把接受的连接分发到各个 loop。两者的读写热路径是同一个
// StreamConnection。
// 接受前过滤（AcceptFilter）、水位、idle 等选项是 StreamServer 的事，这里不做。

#pragma once

#include <memory>

#include "base/net/codec/frame_codec_interface.h"
#include "base/net/connection/connection_handler_interface.h"
#include "base/net/loop/event_loop.h"
#include "base/net/socket/unique_fd.h"
#include "base/net/stream/internal/acceptor.h"
#include "base/net/tcp/tcp_options.h"

namespace breeze
{

class LoopServer
{
public:
    // handler 与 factory 只持引用，调用方保证它们比 LoopServer 与它的连接活得久。
    LoopServer(EventLoop& loop,
               Endpoint endpoint,
               IConnectionHandler& handler,
               const IFrameCodecFactory& factory,
               ConnectionOptions options = {},
               TcpOptions tcp = {});

    // 在 loop 线程上调用（loop 开始运行之前的装配阶段不限）。返回 errno，失败时已回滚。
    [[nodiscard]] int Start() { return acceptor_.Listen(); }
    void Stop() { acceptor_.Stop(); }

    // 实际绑定的地址：端口填 0 时可以从这里取到系统分配的端口。
    const Endpoint& endpoint() const noexcept { return acceptor_.endpoint(); }
    const Acceptor& acceptor() const noexcept { return acceptor_; }

private:
    void OnAccept(UniqueFd fd, std::optional<Endpoint> peer);

    EventLoop& loop_;
    IConnectionHandler& handler_;
    const IFrameCodecFactory& factory_;
    ConnectionOptions options_;
    TcpOptions tcp_;
    Acceptor acceptor_; // 声明在最后：先于上面的成员销毁，销毁时 Stop 仍可用
};

} // namespace breeze
