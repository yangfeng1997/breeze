#include "base/net/stream/internal/acceptor.h"

#include <fcntl.h>
#include <sys/epoll.h>

#include <cerrno>

#include "base/logger/logger.h"
#include "base/logger/logger_macros.h"
#include "base/net/stream/internal/stream_socket.h"
#include "base/net/tcp/tcp_socket.h"
#include "base/net/uds/uds_socket.h"

namespace breeze
{

namespace
{

UniqueFd OpenIdleFd() { return UniqueFd(::open("/dev/null", O_RDONLY | O_CLOEXEC)); }

} // namespace

Acceptor::Acceptor(EventLoop& loop, Endpoint endpoint, OnAccept on_accept)
    : loop_(loop)
    , endpoint_(std::move(endpoint))
    , on_accept_(std::move(on_accept))
{
}

Acceptor::~Acceptor() { Stop(); }

int Acceptor::Listen(int backlog)
{
    loop_.AssertInLoopThread();
    if (listening_)
        LOG_FATAL("Acceptor 重复 Listen endpoint={}", endpoint_.ToString());

    // 局部的 UniqueFd 在任一步失败时自动关闭，所以不需要手写回滚。
    UniqueFd fd;
    if (const int e = sock::CreateStreamSocket(endpoint_, fd); e != 0)
        return e;
    if (endpoint_.kind() == Endpoint::Kind::Tcp)
    {
        if (const int e = tcp::SetReuseAddr(fd.get()); e != 0)
            return e;
    }
    if (const int e = sock::Bind(fd.get(), endpoint_); e != 0)
        return e;
    if (const int e = sock::Listen(fd.get(), backlog); e != 0)
        return e;

    UniqueFd idle = OpenIdleFd();
    if (!idle.valid())
        return errno;
    if (const int e = loop_.Add(this, fd.get(), EPOLLIN); e != 0)
        return e;

    std::optional<Endpoint> local;
    if (sock::LocalEndpoint(fd.get(), local) == 0 && local)
        endpoint_ = *local;

    listen_fd_ = std::move(fd);
    idle_fd_ = std::move(idle);
    listening_ = true;
    return 0;
}

void Acceptor::Stop()
{
    if (!listening_)
        return;
    loop_.Del(listen_fd_.get());
    if (endpoint_.kind() == Endpoint::Kind::Unix)
        uds::RemoveSocketFile(endpoint_);
    listen_fd_.reset();
    idle_fd_.reset();
    listening_ = false;
}

void Acceptor::OnEvents(std::uint32_t)
{
    for (int i = 0; i < kMaxAcceptPerEvent; ++i)
    {
        UniqueFd fd;
        std::optional<Endpoint> peer;
        const int err = sock::Accept(listen_fd_.get(), fd, peer);
        if (err == 0)
        {
            on_accept_(std::move(fd), std::move(peer));
            continue;
        }
        switch (err)
        {
            case EAGAIN:
                return;
            case EMFILE:
            case ENFILE:
                HandleFdExhausted();
                continue;
            case ECONNABORTED: // 对端在我们 accept 之前就放弃了，不是我们的错
            case EPROTO:
            case EPERM: // 防火墙规则拒绝了这条连接
                continue;
            default:
                LOG_ERROR("accept 失败 endpoint={} errno={}", endpoint_.ToString(), err);
                return;
        }
    }
}

// 水平触发下，fd 耗尽时待接受的连接会让监听 fd 一直就绪，accept 又一直失败：loop 空转。
// 办法：先关掉预留的 fd 腾出一个名额，把这条连接接出来立即关掉（对端得到 EOF 而不是挂起），再把名额占回来。
void Acceptor::HandleFdExhausted()
{
    idle_fd_.reset();
    {
        UniqueFd victim;
        std::optional<Endpoint> peer;
        if (sock::Accept(listen_fd_.get(), victim, peer) == 0)
        {
            rejected_fd_exhausted_.fetch_add(1, std::memory_order_relaxed); // victim 离开作用域即关闭
            LoopStats::Bump(loop_.stats().rejected_fd_exhausted);
        }
    }
    idle_fd_ = OpenIdleFd();

    // 日志限频：fd 耗尽时每秒可能有成千上万条连接涌入。
    const auto now = EventLoop::Clock::now();
    if (now - last_exhausted_log_ >= std::chrono::seconds(1))
    {
        last_exhausted_log_ = now;
        LOG_WARN("fd 耗尽，拒绝新连接 endpoint={} 累计={}", endpoint_.ToString(), rejected_fd_exhausted());
    }
}

} // namespace breeze
