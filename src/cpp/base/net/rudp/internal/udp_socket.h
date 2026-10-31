// 注册在 EventLoop 上的 UDP socket：批量收（recvmmsg）、分片发（sendmsg，两个 iovec 不拷贝）。
// 服务端一个 socket 服务所有会话（不 connect，按来源地址分发）；
// 客户端的 socket connect 到服务端（内核只放行该来源的包）。
// 设计见 docs/网络库设计.md §4.14。
//
// 只能在所属 EventLoop 的线程上使用。UDP 不可靠，发送失败（缓冲满 EAGAIN / ENOBUFS）就丢：KCP 会重传。
// 析构只关 fd（epoll 会自动摘掉）而不碰 loop：loop 可能已经先销毁了；要在 loop 还活着时摘掉并关掉，调 Close()。

#pragma once

#include <sys/socket.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

#include "endpoint.h"
#include "internal/core/event_loop.h"
#include "internal/core/pollable.h"
#include "internal/core/unique_fd.h"
#include "rudp_options.h"

namespace breeze
{

class DatagramHandler
{
public:
    // 收到一个数据报。data 只在回调内有效。回调里不能销毁 socket。
    virtual void OnDatagram(const sockaddr_storage& src, socklen_t src_len, std::span<const std::byte> data) = 0;
    // 内核报告的 socket 错误（EPOLLERR 里的 SO_ERROR，或 recv 的 ECONNREFUSED 这类 ICMP 错误），已清除，socket 仍可用。
    virtual void OnSocketError(int /*sys_errno*/) {}

protected:
    ~DatagramHandler() = default;
};

class UdpSocket final : public Pollable
{
public:
    // 服务端：bind 到 local。成功时 out 有值，失败返回 errno（out 不变）。
    [[nodiscard]] static int OpenServer(EventLoop& loop,
                                        const Endpoint& local,
                                        const RudpOptions& options,
                                        DatagramHandler& handler,
                                        std::unique_ptr<UdpSocket>& out);
    // 客户端：connect 到 peer（UDP 的 connect 只设默认对端，立即返回）。
    [[nodiscard]] static int OpenClient(EventLoop& loop,
                                        const Endpoint& peer,
                                        const RudpOptions& options,
                                        DatagramHandler& handler,
                                        std::unique_ptr<UdpSocket>& out);

    ~UdpSocket() override = default;

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    // 发一个数据报：prefix 与 payload 拼成一个包（任一可以为空）。to 为空 = 用 connect 的对端。
    // 返回 0 或 errno；EAGAIN / ENOBUFS 也是错误码，调用方按丢包处理，不要重试。
    int Send(std::span<const std::byte> prefix,
             std::span<const std::byte> payload,
             const sockaddr_storage* to,
             socklen_t to_len) noexcept;

    // 实际绑定的本地地址（端口填 0 时是系统分配的）。
    const Endpoint& local() const noexcept { return local_; }
    int fd() const noexcept { return fd_.get(); }
    bool open() const noexcept { return fd_.valid(); }

    // 摘出 epoll 并关闭 fd。幂等。
    void Close();

private:
    UdpSocket(EventLoop& loop, UniqueFd fd, Endpoint local, DatagramHandler& handler)
        : loop_(loop)
        , fd_(std::move(fd))
        , handler_(handler)
        , local_(std::move(local))
    {
    }

    static int Open(EventLoop& loop,
                    const Endpoint& ep,
                    bool server,
                    const RudpOptions& options,
                    DatagramHandler& handler,
                    std::unique_ptr<UdpSocket>& out);

    void OnEvents(std::uint32_t events) override;
    // 读一批，返回收到的数据报个数；0 = 没有更多。
    int ReadBatch();

    EventLoop& loop_;
    UniqueFd fd_;
    DatagramHandler& handler_;
    Endpoint local_;
    bool registered_ = false;
};

} // namespace breeze
