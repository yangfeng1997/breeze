#include "udp_socket.h"

#include <sys/epoll.h>
#include <sys/uio.h>

#include <cerrno>
#include <cstring>

#include "logger.h"
#include "logger_macros.h"
#include "socket_address.h"

namespace breeze
{

namespace
{

// 一次 recvmmsg 的数据报数，与一次事件里最多读几批。读不完的下一轮 epoll 水平触发再来，所以只是公平性上限：
// 一个繁忙的 socket 不能饿死同一个 loop 上的其它连接与定时器（含 KCP 的 Update）。
constexpr unsigned kBatch = 16;
constexpr int kMaxBatchesPerEvent = 4;
// 多留 1 字节：收到 kMaxMtu + 1 就说明对端的数据报比我们允许的大，丢掉。
constexpr std::size_t kSlot = rudp::kMaxMtu + 1;

void SetBuffer(int fd, int opt, std::uint32_t bytes)
{
    if (bytes == 0)
        return;
    const int v = static_cast<int>(bytes);
    // 失败不致命（权限、被 rmem_max 截断都不报错），用系统给的就行。
    if (::setsockopt(fd, SOL_SOCKET, opt, &v, sizeof(v)) != 0)
        LOG_DEBUG("setsockopt(buffer) 失败 fd={} errno={}", fd, errno);
}

// 读 SO_ERROR 并清除；getsockopt 自己失败时返回它的 errno。
int TakeSocketError(int fd)
{
    int err = 0;
    socklen_t len = sizeof(err);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0)
        return errno;
    return err;
}

int BindTo(int fd, const Endpoint& ep)
{
    sockaddr_storage ss;
    const socklen_t len = sock::ToSockaddr(ep, ss);
    return ::bind(fd, reinterpret_cast<const sockaddr*>(&ss), len) == 0 ? 0 : errno;
}

// UDP 的 connect 只设默认对端，不握手，立即返回。
int ConnectTo(int fd, const Endpoint& ep)
{
    sockaddr_storage ss;
    const socklen_t len = sock::ToSockaddr(ep, ss);
    return ::connect(fd, reinterpret_cast<const sockaddr*>(&ss), len) == 0 ? 0 : errno;
}

} // namespace

int UdpSocket::OpenServer(EventLoop& loop,
                          const Endpoint& local,
                          const RUdpOptions& options,
                          DatagramHandler& handler,
                          std::unique_ptr<UdpSocket>& out)
{
    return Open(loop, local, true, options, handler, out);
}

int UdpSocket::OpenClient(EventLoop& loop,
                          const Endpoint& peer,
                          const RUdpOptions& options,
                          DatagramHandler& handler,
                          std::unique_ptr<UdpSocket>& out)
{
    return Open(loop, peer, false, options, handler, out);
}

int UdpSocket::Open(EventLoop& loop,
                    const Endpoint& ep,
                    bool server,
                    const RUdpOptions& options,
                    DatagramHandler& handler,
                    std::unique_ptr<UdpSocket>& out)
{
    loop.AssertInLoopThread();
    if (ep.kind() != Endpoint::Kind::Udp)
        return EPROTONOSUPPORT;
    UniqueFd fd;
    if (const int e = sock::CreateSocket(ep, SOCK_DGRAM, fd); e != 0)
        return e;
    SetBuffer(fd.get(), SO_RCVBUF, options.socket_recv_buffer);
    SetBuffer(fd.get(), SO_SNDBUF, options.socket_send_buffer);
    if (const int e = server ? BindTo(fd.get(), ep) : ConnectTo(fd.get(), ep); e != 0)
        return e;

    // 本地地址：服务端取系统分配的端口；客户端取内核选的源地址（日志用）。
    sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    Endpoint local = ep;
    if (::getsockname(fd.get(), reinterpret_cast<sockaddr*>(&ss), &len) == 0)
    {
        if (auto got = sock::FromSockaddr(ss, len, Endpoint::Kind::Udp))
            local = std::move(*got);
    }

    std::unique_ptr<UdpSocket> s(new UdpSocket(loop, std::move(fd), std::move(local), handler));
    if (const int e = loop.Add(s.get(), s->fd_.get(), EPOLLIN); e != 0)
        return e;
    s->registered_ = true;
    out = std::move(s);
    return 0;
}

void UdpSocket::Close()
{
    if (!fd_.valid())
        return;
    if (registered_)
    {
        loop_.Del(fd_.get());
        registered_ = false;
    }
    fd_.reset();
}

int UdpSocket::Send(std::span<const std::byte> prefix,
                    std::span<const std::byte> payload,
                    const sockaddr_storage* to,
                    socklen_t to_len) noexcept
{
    if (!fd_.valid())
        return EBADF;
    iovec iov[2];
    int cnt = 0;
    if (!prefix.empty())
        iov[cnt++] = {const_cast<std::byte*>(prefix.data()), prefix.size()};
    if (!payload.empty())
        iov[cnt++] = {const_cast<std::byte*>(payload.data()), payload.size()};
    msghdr msg{};
    msg.msg_iov = iov;
    msg.msg_iovlen = static_cast<std::size_t>(cnt);
    if (to != nullptr)
    {
        msg.msg_name = const_cast<sockaddr_storage*>(to);
        msg.msg_namelen = to_len;
    }
    for (;;)
    {
        const ssize_t n = ::sendmsg(fd_.get(), &msg, MSG_NOSIGNAL);
        if (n >= 0)
        {
            LoopStats::Bump(loop_.stats().bytes_out, static_cast<std::uint64_t>(n));
            return 0;
        }
        if (errno != EINTR)
            return errno;
    }
}

void UdpSocket::OnEvents(std::uint32_t events)
{
    if (events & EPOLLERR)
    {
        // connect 过的 UDP socket 收到 ICMP 不可达会报在这里。必须读掉 SO_ERROR，否则水平触发的 EPOLLERR 一直就绪。
        if (const int err = TakeSocketError(fd_.get()); err != 0)
            handler_.OnSocketError(err);
    }
    if (!(events & EPOLLIN))
        return;
    for (int i = 0; i < kMaxBatchesPerEvent && fd_.valid(); ++i)
    {
        if (ReadBatch() < static_cast<int>(kBatch))
            return; // 没读满：内核缓冲已空
    }
}

int UdpSocket::ReadBatch()
{
    // 线程本地：同一线程上所有 socket 共用，回调里不会再进入读路径。
    static thread_local std::byte bufs[kBatch][kSlot];
    static thread_local sockaddr_storage addrs[kBatch];
    static thread_local iovec iov[kBatch];
    static thread_local mmsghdr msgs[kBatch];

    // iov / name / iovlen 是固定的，只在本线程第一次进来时填；
    // 每次都要重置的只有 msg_namelen（内核会把它改写成实际地址长度）。
    // 每批都把 16 个头整个重写一遍，在压测里占服务端用户态指令的 4–6%。
    static thread_local bool inited = false;
    if (!inited)
    {
        for (unsigned i = 0; i < kBatch; ++i)
        {
            iov[i] = {bufs[i], kSlot};
            msgs[i] = {};
            msgs[i].msg_hdr.msg_iov = &iov[i];
            msgs[i].msg_hdr.msg_iovlen = 1;
            msgs[i].msg_hdr.msg_name = &addrs[i];
        }
        inited = true;
    }
    for (unsigned i = 0; i < kBatch; ++i)
        msgs[i].msg_hdr.msg_namelen = sizeof(sockaddr_storage);
    int n;
    do
    {
        n = ::recvmmsg(fd_.get(), msgs, kBatch, MSG_DONTWAIT, nullptr);
    } while (n < 0 && errno == EINTR);
    if (n < 0)
    {
        // ICMP 错误作为 recv 的返回值报告一次（并清除）。其它（EAGAIN）就是读空了。
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            handler_.OnSocketError(errno);
        return 0;
    }
    for (int i = 0; i < n && fd_.valid(); ++i)
    {
        const std::size_t len = msgs[i].msg_len;
        if (len >= kSlot || (msgs[i].msg_hdr.msg_flags & MSG_TRUNC))
            continue; // 比允许的最大数据报还大：丢
        LoopStats::Bump(loop_.stats().bytes_in, len);
        handler_.OnDatagram(addrs[i], msgs[i].msg_hdr.msg_namelen, {bufs[i], len});
    }
    return n;
}

} // namespace breeze
