// 监听一个 endpoint，accept 之后把连接分到 group 里连接数最少的 loop 上。设计见 docs/网络库设计.md §4.1 / §4.4 / §4.8。
//
// TCP 与 UDS 共用的服务端基类：accept、分配 loop、过滤、停机的逻辑都在这里。
// 业务代码用 TcpServer（tcp/）或 UdsServer（uds/），它们收类型化的端点与选项；这个基类收通用的 Endpoint 与
// StreamServerOptions，给"按端点种类分派"的代码用（NetModule、测试），也是它们的统一句柄（NetModule 经基类指针持有）。
//
// 一个对象一个 endpoint：gate 要同时监听 TCP 与 UDS，就建一个 TcpServer 和一个 UdsServer、共用一个 EventLoopGroup。
// Acceptor 挂在 Start() 时选定的那个 loop 上（也在那个 loop 上服务连接）。
//
// 生命周期：handler 与 factory 只持引用，调用方保证它们比 EventLoopGroup::Shutdown 活得久（Shutdown 之后不再有回调）；
// StreamServer 对象自己也必须覆盖到 Shutdown 返回之后——Stop() 是异步的，loop 上可能仍有引用 this 的任务。
// 析构时若 group 仍在运行，会先同步停掉 accept（保险，不是用法）。

#pragma once

#include <sys/socket.h>

#include <functional>
#include <memory>

#include "base/net/codec/frame_codec_interface.h"
#include "base/net/connection/connection_handler_interface.h"
#include "base/net/connection/connection_options.h"
#include "base/net/connection/net_server_interface.h"
#include "base/net/endpoint/endpoint.h"
#include "base/net/endpoint/peer_info.h"
#include "base/net/loop/event_loop_group.h"
#include "base/net/tcp/tcp_options.h"

namespace breeze
{

struct StreamServerOptions
{
    ConnectionOptions connection; // 水位、idle_timeout：对每条被接受的连接生效
    TcpOptions tcp; // keepalive 等：只对 TCP 端点生效，UDS 端点不看它（TcpServer / UdsServer 的选项里按传输分开）
    AcceptFilter accept_filter; // 空 = 全部接受
    int backlog = SOMAXCONN;    // 内核还会按 net.core.somaxconn 截断
};

class StreamServer : public INetServer
{
public:
    StreamServer(EventLoopGroup& group,
                 const Endpoint& endpoint,
                 StreamServerOptions options,
                 IConnectionHandler& handler,
                 const IFrameCodecFactory& factory);
    ~StreamServer() override; // TcpServer / UdsServer 经基类指针销毁

    StreamServer(const StreamServer&) = delete;
    StreamServer& operator=(const StreamServer&) = delete;

    // group 必须已 Start（否则是编程错误，LOG_FATAL）；只能调用一次。阻塞到 bind / listen 在 IO 线程上完成。
    // 0 = 成功，否则 errno（EADDRINUSE、EACCES、ENAMETOOLONG……），失败时已回滚；group 正在 / 已经停机返回 ESHUTDOWN；
    // 端点是 UDP（数据报）返回 EPROTONOSUPPORT：字节流的服务端不收它。
    [[nodiscard]] int Start() override;

    // 只停 accept，已接受的连接继续服务，由 EventLoopGroup::Shutdown 收尾。异步；对没 Start 过的对象是空操作。
    void Stop() override;

    // 实际绑定的地址：端口填 0 时 Start 成功之后这里是系统分配的端口。Start 之前就是构造时给的 endpoint。
    const Endpoint& endpoint() const noexcept override { return endpoint_; }

private:
    struct Impl;

    Endpoint endpoint_;
    std::unique_ptr<Impl> impl_;
};

} // namespace breeze
