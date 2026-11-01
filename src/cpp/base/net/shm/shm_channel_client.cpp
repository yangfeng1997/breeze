#include "base/net/shm/shm_channel_client.h"

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstring>
#include <span>
#include <thread>
#include <type_traits>
#include <utility>

#include "base/net/shm/shm_protocol.h"
#include "base/net/stream/internal/fd_passing.h"
#include "base/net/stream/internal/stream_socket.h"

namespace breeze
{

namespace
{

using Clock = std::chrono::steady_clock;

int RemainingMs(Clock::time_point deadline)
{
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    if (left <= 0)
        return 0;
    return left > INT_MAX ? INT_MAX : static_cast<int>(left);
}

// 等 fd 就绪，至多到 deadline。0 = 就绪（POLLERR / POLLHUP 也算：交给后面的读写去发现具体错误），否则 errno。
int WaitFd(int fd, short events, Clock::time_point deadline)
{
    for (;;)
    {
        pollfd p{fd, events, 0};
        const int n = ::poll(&p, 1, RemainingMs(deadline));
        if (n > 0)
            return 0;
        if (n == 0)
            return ETIMEDOUT;
        if (errno == EINTR)
            continue;
        return errno;
    }
}

int ConnectUds(int fd, const Endpoint& ep, Clock::time_point deadline)
{
    for (;;)
    {
        const int e = sock::Connect(fd, ep);
        if (e == 0)
            return 0;
        if (e == EINPROGRESS)
        {
            if (const int w = WaitFd(fd, POLLOUT, deadline); w != 0)
                return w;
            return sock::GetSocketError(fd);
        }
        if (e == EAGAIN) // UDS 的 backlog 满了：非阻塞 connect 会这样，稍等重试
        {
            if (Clock::now() >= deadline)
                return ETIMEDOUT;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        return e;
    }
}

int SendAll(int fd, std::span<const std::byte> data, Clock::time_point deadline)
{
    std::size_t off = 0;
    while (off < data.size())
    {
        const ssize_t n = ::send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
        if (n >= 0)
        {
            off += static_cast<std::size_t>(n);
            continue;
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            if (const int w = WaitFd(fd, POLLOUT, deadline); w != 0)
                return w;
            continue;
        }
        return errno;
    }
    return 0;
}

} // namespace

namespace
{

// 一次握手的结果：连上的 UDS、校验过头的 Reply、随 Reply 到达的 fd。
struct Handshake
{
    UniqueFd sock;
    shm::wire::Reply reply{};
    std::array<UniqueFd, 4> fds; // 比期望的 3 个多留一个槽位：对端多塞会被当成协议错误
    std::size_t nfds = 0;
};

// 发 Hello（Reattach 时随它附上 fd）。socket 是非阻塞的，Hello 只有 112 字节，一般一次发完；
// 缓冲满了（EAGAIN）时整条都没发出去，fd 也没发，重试是安全的。
int SendHello(int fd, const shm::wire::Hello& hello, std::span<const int> fds, Clock::time_point deadline)
{
    for (;;)
    {
        const int e = sock::SendWithFds(fd, std::as_bytes(std::span(&hello, 1)), fds);
        if (e != EAGAIN && e != EWOULDBLOCK)
            return e;
        if (const int w = WaitFd(fd, POLLOUT, deadline); w != 0)
            return w;
    }
}

// 连接 → 发 Hello → 收 Reply。result != 0 的 Reply（Reject）也当错误返回，原因就是 errno。
std::expected<Handshake, int> DoHandshake(const ShmClientOptions& o,
                                          shm::wire::AttachType type,
                                          std::span<const int> send_fds,
                                          Clock::time_point deadline)
{
    using namespace shm;
    wire::Hello hello{};
    hello.hdr = wire::MakeHeader(wire::MsgType::kHello);
    hello.attach_type = static_cast<std::uint32_t>(type);
    hello.transport = static_cast<std::uint32_t>(wire::Transport::kShmRequired);
    hello.pid = static_cast<std::int32_t>(::getpid());
    if (type == wire::AttachType::kAttach)
    {
        hello.flags = o.discard_retained ? wire::kFlagDiscardRetained : 0;
        hello.tx_capacity = o.tx_capacity;
        hello.rx_capacity = o.rx_capacity;
        hello.tx_max_payload = o.tx_max_payload;
        hello.rx_max_payload = o.rx_max_payload;
    }
    if (!wire::SetBusId(hello, o.busid))
        return std::unexpected(EINVAL);

    Handshake h;
    if (const int e = sock::CreateStreamSocket(o.endpoint, h.sock); e != 0)
        return std::unexpected(e);
    if (const int e = ConnectUds(h.sock.get(), o.endpoint, deadline); e != 0)
        return std::unexpected(e);
    if (const int e = SendHello(h.sock.get(), hello, send_fds, deadline); e != 0)
        return std::unexpected(e);

    // 收 Reply：定长，fd（如果有）随第一段数据一起到。
    std::array<std::byte, sizeof(wire::Reply)> buf{};
    std::size_t got = 0;
    while (got < buf.size())
    {
        std::array<UniqueFd, 4> chunk;
        std::size_t n = 0, nf = 0;
        const int e = sock::RecvWithFds(h.sock.get(), std::span(buf).subspan(got), n, chunk, nf);
        if (e == EAGAIN || e == EWOULDBLOCK)
        {
            if (const int w = WaitFd(h.sock.get(), POLLIN, deadline); w != 0)
                return std::unexpected(w);
            continue;
        }
        if (e != 0)
            return std::unexpected(e == EMSGSIZE ? EPROTO : e); // 附带的 fd 太多 = 协议错误
        if (n == 0)
            return std::unexpected(ECONNRESET); // 没回复就关了
        got += n;
        for (std::size_t i = 0; i < nf; ++i)
        {
            if (h.nfds >= h.fds.size())
                return std::unexpected(EPROTO);
            h.fds[h.nfds++] = std::move(chunk[i]);
        }
    }
    std::memcpy(&h.reply, buf.data(), sizeof(h.reply)); // 先拷到本地再校验
    if (const int e = wire::CheckReplyHeader(h.reply); e != 0)
        return std::unexpected(e);
    if (h.reply.result != 0)
        return std::unexpected(h.reply.result); // Reject：原因就是 errno（附带的 fd 如果有，随 h 析构关闭）
    return h;
}

// 把 Reply 里的布局与本地从共享内存头算出来的互相核对：对不上说明 Agent 或共享内存不对劲。
bool LayoutMatches(const shm::wire::Reply& r, const shm::ChannelLayout& l)
{
    return r.total_size == l.total_size && r.tx.capacity == l.tx.capacity && r.tx.max_payload == l.tx.max_payload &&
           r.rx.capacity == l.rx.capacity && r.rx.max_payload == l.rx.max_payload && r.tx.reserved == 0 &&
           r.rx.reserved == 0 && r.reserved == 0;
}

// EBUSY 的重试：间隔从 5ms 起每次翻倍、至多 100ms，直到 busy_retry 窗口（或总时限）结束；最后一次尝试恰在窗口结束处。
template <typename Attempt>
auto RetryBusy(const ShmClientOptions& o, Clock::time_point deadline, Attempt&& attempt)
{
    const auto retry_until = std::min(deadline, Clock::now() + o.busy_retry);
    auto delay = std::chrono::milliseconds(5);
    for (;;)
    {
        auto r = attempt();
        const int err = [&]
        {
            if constexpr (std::is_same_v<decltype(r), int>)
                return r;
            else
                return r ? 0 : r.error();
        }();
        const auto now = Clock::now();
        if (err != EBUSY || now >= retry_until)
            return r;
        std::this_thread::sleep_for(
            std::min<Clock::duration>(delay, retry_until - now));
        delay = std::min(delay * 2, std::chrono::milliseconds(100));
    }
}

} // namespace

std::expected<ShmChannelClient, int> ShmChannelClient::Connect(const ShmClientOptions& o)
{
    using namespace shm;
    if (o.endpoint.kind() != Endpoint::Kind::Unix && o.endpoint.kind() != Endpoint::Kind::UnixAbstract)
        return std::unexpected(EPROTONOSUPPORT);
    if (!wire::IsValidBusId(o.busid))
        return std::unexpected(EINVAL);

    const auto deadline = Clock::now() + o.timeout;
    auto hs = RetryBusy(o, deadline, [&] { return DoHandshake(o, wire::AttachType::kAttach, {}, deadline); });
    if (!hs)
        return std::unexpected(hs.error());
    if (hs->nfds != 3)
        return std::unexpected(EPROTO);
    if (hs->reply.transport != static_cast<std::uint32_t>(wire::ChosenTransport::kShm))
        return std::unexpected(EPROTO); // 我们要的是共享内存，Agent 回了别的

    // 接手：Attach 会校验封印、大小、头部、布局、容量范围与游标；它总是接管 fd，失败时也会关闭。
    auto chan = ShmChannel::Attach(
        Fd(hs->fds[0].release()), Fd(hs->fds[1].release()), Fd(hs->fds[2].release()), o.limits);
    if (!chan)
        return std::unexpected(chan.error());
    if (!LayoutMatches(hs->reply, chan->layout()) || hs->reply.resumed > 1)
        return std::unexpected(EPROTO);

    ShmChannelClient c;
    c.sock_ = std::move(hs->sock);
    c.chan_ = std::move(*chan);
    c.tx_ = c.chan_.TxProducer();
    c.rx_ = c.chan_.RxConsumer();
    if (c.tx_.corrupt() || c.rx_.corrupt())
        return std::unexpected(EINVAL);
    c.app_epoch_ = hs->reply.app_epoch;
    c.agent_epoch_ = hs->reply.agent_epoch;
    c.resumed_ = hs->reply.resumed == 1;
    return c;
}

int ShmChannelClient::Reattach(const ShmClientOptions& o)
{
    using namespace shm;
    if (!chan_.valid())
        return EINVAL;
    if (o.endpoint.kind() != Endpoint::Kind::Unix && o.endpoint.kind() != Endpoint::Kind::UnixAbstract)
        return EPROTONOSUPPORT;
    if (!wire::IsValidBusId(o.busid))
        return EINVAL;

    const auto deadline = Clock::now() + o.timeout;
    const std::array<int, 3> fds{chan_.memfd(), chan_.tx_eventfd(), chan_.rx_eventfd()};
    auto hs = RetryBusy(o, deadline, [&] { return DoHandshake(o, wire::AttachType::kReattach, fds, deadline); });
    if (!hs)
        return hs.error();
    // Reattach 的 Accept 不带 fd（我们手里本来就有）；对得上才算数。
    if (hs->nfds != 0 || hs->reply.resumed != 1 ||
        hs->reply.transport != static_cast<std::uint32_t>(wire::ChosenTransport::kShm) ||
        !LayoutMatches(hs->reply, chan_.layout()))
        return EPROTO;
    // 通道已被别的进程接管：本进程不该再用它。
    if (hs->reply.app_epoch != app_epoch_ || Superseded())
        return ESTALE;
    // 新 Agent 接手时 agent_epoch 加一；Agent 没重启（还留着这条通道）时不变；倒退说明不对劲。
    if (hs->reply.agent_epoch < agent_epoch_)
        return EPROTO;
    sock_ = std::move(hs->sock);
    agent_epoch_ = hs->reply.agent_epoch;
    resumed_ = true;
    return 0;
}

int ShmChannelClient::SendPrepareRestart(std::chrono::milliseconds retain)
{
    using namespace shm;
    if (!sock_.valid())
        return ENOTCONN;
    if (retain.count() <= 0 || retain.count() > static_cast<std::chrono::milliseconds::rep>(UINT32_MAX))
        return EINVAL;
    const wire::Control c = wire::MakeControl(wire::ControlOp::kPrepareRestart, static_cast<std::uint32_t>(retain.count()));
    return SendAll(sock_.get(), std::as_bytes(std::span(&c, 1)), Clock::now() + std::chrono::seconds(1));
}

int ShmChannelClient::SendBye()
{
    using namespace shm;
    if (!sock_.valid())
        return ENOTCONN;
    const wire::Control c = wire::MakeControl(wire::ControlOp::kBye, 0);
    return SendAll(sock_.get(), std::as_bytes(std::span(&c, 1)), Clock::now() + std::chrono::seconds(1));
}

bool ShmChannelClient::Superseded() const noexcept
{
    return chan_.valid() && chan_.header().app_epoch.load(std::memory_order_acquire) != app_epoch_;
}

bool ShmChannelClient::AgentGone() noexcept
{
    if (!sock_.valid())
        return true;
    std::array<std::byte, 64> sink;
    for (;;)
    {
        const ssize_t n = ::recv(sock_.get(), sink.data(), sink.size(), MSG_DONTWAIT);
        if (n != 0 && n != -1)
            return true; // 握手之后 Agent 不该往 UDS 里写任何东西：当作断开
        if (n == 0)
            return true; // EOF
        if (errno == EINTR)
            continue;
        return !(errno == EAGAIN || errno == EWOULDBLOCK);
    }
}

} // namespace breeze
