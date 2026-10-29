#include "connection.h"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cerrno>

#include "event_loop.h"
#include "logger.h"
#include "logger_macros.h"

namespace breeze
{

namespace
{

// 一次 read 最多收的字节数。读缓冲是线程本地的，所有连接共用（IO 线程一次只服务一条连接，回调里也不会再进入读路径）。
constexpr std::size_t kReadBytes = 64 * 1024;
// 每连接的缓冲只存"半个帧"。读空后若它已长到这么大就释放，空闲连接不占内存；
// 带滞后（小于它的留着），避免稳定的跨读帧流量反复分配释放。
constexpr std::size_t kShrinkAbove = 4 * 1024;
// 一次 sendmsg 最多 64 个 iovec（一帧占两个：帧头 + payload）。
constexpr int kMaxIov = 64;

// 返回写出的字节数（0 = 内核缓冲满了）；出错返回 -1，errno 有效。
// 用 sendmsg + MSG_NOSIGNAL 而不是 writev：对端已关闭时 writev 会向进程发 SIGPIPE，库不该替使用方改信号处置。
ssize_t SendIov(int fd, iovec* iov, int cnt)
{
    msghdr msg{};
    msg.msg_iov = iov;
    msg.msg_iovlen = static_cast<std::size_t>(cnt);
    for (;;)
    {
        const ssize_t n = ::sendmsg(fd, &msg, MSG_NOSIGNAL);
        if (n >= 0)
            return n;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        return -1;
    }
}

} // namespace

Connection::Connection(EventLoop& loop,
                       Fd fd,
                       PeerInfo peer,
                       ConnectionHandler& handler,
                       std::unique_ptr<FrameCodec> codec,
                       const ConnectionOptions& options,
                       ConnectionTable::Slot& slot,
                       std::uint32_t slot_index,
                       ConnectionHandle handle)
    : loop_(loop)
    , fd_(std::move(fd))
    , peer_(std::move(peer))
    , handler_(handler)
    , codec_(std::move(codec))
    , slot_(slot)
    , slot_index_(slot_index)
    , handle_(handle)
    , high_water_(options.high_water_mark)
{
}

Connection::~Connection() = default;

bool Connection::Establish()
{
    loop_.AssertInLoopThread();
    if (const int err = loop_.Add(this, fd_.get(), EPOLLIN | EPOLLRDHUP); err != 0)
    {
        LOG_WARN("epoll_ctl(ADD) 失败 fd={} errno={}，只关闭这条新连接", fd_.get(), err);
        state_ = State::Disconnected;
        slot_.state.store(ConnectionTable::State::Disconnected, std::memory_order_release);
        loop_.QueueClose(this); // 没回调过 OnConnected，回收时也就不会有 OnClosed
        return false;
    }
    registered_ = true;
    notified_connected_ = true;
    handler_.OnConnected(*this);
    return true;
}

// ---- 事件分发：HUP（无 IN）→ ERR → IN/RDHUP → OUT ----

void Connection::OnEvents(std::uint32_t events)
{
    // 同一轮里本连接可能已经在别的回调里被关闭：对象还在（延后销毁），但不再处理任何事件。
    if (state_ != State::Connected)
        return;
    if ((events & EPOLLHUP) && !(events & EPOLLIN))
    {
        CloseWith(CloseReason::PeerClosed, 0);
        return;
    }
    if (events & EPOLLERR)
    {
        const int err = sock::SoError(fd_.get());
        CloseWith(CloseReason::IoError, err != 0 ? err : EIO);
        return;
    }
    if (events & (EPOLLIN | EPOLLRDHUP))
        HandleRead();
    if (state_ == State::Connected && (events & EPOLLOUT))
        HandleWrite();
}

// ---- 读：水平触发，每次就绪只 read 一次 ----

// 从 data 开头起把能解的帧都解出来，返回消耗的字节数。一次解一帧：两帧之间要检查连接是否已经在回调里被关闭。
std::size_t Connection::DecodeAll(std::span<const std::byte> data)
{
    std::size_t total = 0;
    while (state_ == State::Connected && total < data.size())
    {
        std::size_t used = 0;
        const DecodeResult r = codec_->Decode(
            data.subspan(total),
            [this](std::span<const std::byte> payload) { handler_.OnMessage(*this, payload); },
            used);
        if (r == DecodeResult::Ok)
        {
            total += used;
            continue;
        }
        if (r == DecodeResult::Error)
            CloseWith(CloseReason::ProtocolError, 0); // 不尝试重新同步
        break;
    }
    return total;
}

void Connection::HandleRead()
{
    static thread_local std::byte buf[kReadBytes];

    ssize_t n;
    do
    {
        n = ::read(fd_.get(), buf, sizeof(buf));
    } while (n < 0 && errno == EINTR);

    if (n < 0)
    {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            CloseWith(CloseReason::IoError, errno);
        return;
    }
    if (n == 0)
    {
        CloseWith(CloseReason::PeerClosed, 0); // 读到 EOF
        return;
    }

    const std::span<const std::byte> fresh(buf, static_cast<std::size_t>(n));
    if (in_.ReadableBytes() == 0)
    {
        // 常见路径：没有遗留的半帧，直接从线程本地缓冲分帧，不碰每连接的缓冲；只有尾部的半个帧才拷进去。
        const std::size_t used = DecodeAll(fresh);
        if (used < fresh.size() && state_ == State::Connected)
            in_.Append(fresh.subspan(used));
        return;
    }

    // 上次留下了半个帧：拼上新数据再解。回调看到的视图在 in_ 里，回调期间 in_ 不会被改动（Send 不碰它）。
    in_.Append(fresh);
    in_.Consume(DecodeAll(in_.Readable()));
    if (in_.ReadableBytes() == 0 && in_.Capacity() > kShrinkAbove)
        in_.ShrinkIfEmpty(0);
}

// ---- 写 ----

namespace
{

// 把 p 还没写出的部分填进 iov，返回填了几个。
int FillIov(
    const std::byte* header, std::size_t header_len, std::span<const std::byte> payload, std::size_t offset, iovec* iov)
{
    int n = 0;
    std::size_t off = offset;
    if (off < header_len)
    {
        iov[n++] = {const_cast<std::byte*>(header) + off, header_len - off};
        off = 0;
    }
    else
    {
        off -= header_len;
    }
    if (off < payload.size())
        iov[n++] = {const_cast<std::byte*>(payload.data()) + off, payload.size() - off};
    return n;
}

} // namespace

SendResult Connection::Send(SharedBuffer payload) { return SendImpl(payload.bytes(), &payload, false); }

SendResult Connection::Send(std::span<const std::byte> payload) { return SendImpl(payload, nullptr, false); }

void Connection::SendFromTask(SharedBuffer payload)
{
    if (state_ != State::Connected)
        return; // 连接已标记关闭：槽回收时 pending_bytes 会清零，这里不用减
    (void)SendImpl(payload.bytes(), &payload, true);
}

// owned 非空：调用方交来的 SharedBuffer，入队时直接接管；为空：payload 只是个视图，入队时才拷贝。
SendResult Connection::SendImpl(std::span<const std::byte> payload, SharedBuffer* owned, bool counted)
{
    loop_.AssertInLoopThread();
    if (state_ != State::Connected)
        return SendResult::Closed;

    std::array<std::byte, kMaxFrameHeaderSize> header; // 不清零：只有 header_len 个字节会被用到
    const std::size_t header_len = codec_->EncodeHeader(payload.size(), header);
    if (header_len == 0)
    {
        // payload 超过 codec 的上限：对端按同样的上限会判协议错误并断开，不如本端先关，别让它带着错位的流继续跑。
        LOG_ERROR("Send 的 payload 超过 codec 上限 size={}，关闭连接", payload.size());
        CloseWith(CloseReason::ProtocolError, 0);
        return SendResult::Closed;
    }

    std::size_t written = 0;
    if (out_.empty())
    {
        // 写队列为空：先直接写，不经过 EPOLLOUT。整帧写完（最常见）就不用构造 Pending。
        iovec iov[2];
        const int cnt = FillIov(header.data(), header_len, payload, 0, iov);
        const ssize_t n = SendIov(fd_.get(), iov, cnt);
        if (n < 0)
        {
            CloseWith(CloseReason::IoError, errno);
            return SendResult::Closed;
        }
        if (static_cast<std::size_t>(n) == header_len + payload.size())
        {
            if (counted)
                slot_.pending_bytes.fetch_sub(payload.size(), std::memory_order_relaxed);
            return SendResult::Ok;
        }
        written = static_cast<std::size_t>(n);
    }

    // 已有积压（排在后面保序，EPOLLOUT 已经注册），或没写完：入队，剩下的靠 EPOLLOUT 排空。
    // 直写整帧成功的快路径不碰 pending_bytes；只有入队才计入，超水位就是慢消费者。
    if (!counted)
    {
        const std::uint64_t pending =
            slot_.pending_bytes.fetch_add(payload.size(), std::memory_order_relaxed) + payload.size();
        if (pending > high_water_)
        {
            LOG_WARN("发送积压 {} 字节超过水位 {}，按慢消费者关闭连接", pending, high_water_);
            CloseWith(CloseReason::SlowConsumer, 0);
            return SendResult::Closed;
        }
    }
    Pending& p = out_.emplace_back();
    p.header = header;
    p.header_len = static_cast<std::uint8_t>(header_len);
    p.payload = owned != nullptr ? std::move(*owned) : SharedBuffer::CopyFrom(payload);
    p.offset = written;
    if (!want_write_)
        SetWriteInterest(true);
    return SendResult::Ok;
}

void Connection::HandleWrite()
{
    while (!out_.empty())
    {
        iovec iov[kMaxIov];
        int cnt = 0;
        std::size_t offered = 0;
        for (auto it = out_.begin(); it != out_.end() && cnt + 2 <= kMaxIov; ++it)
        {
            cnt += FillIov(it->header.data(), it->header_len, it->payload.bytes(), it->offset, iov + cnt);
            offered += it->Total() - it->offset;
        }

        const ssize_t n = SendIov(fd_.get(), iov, cnt);
        if (n < 0)
        {
            CloseWith(CloseReason::IoError, errno);
            return;
        }

        std::size_t left = static_cast<std::size_t>(n);
        while (left > 0)
        {
            Pending& f = out_.front();
            const std::size_t remain = f.Total() - f.offset;
            if (left >= remain)
            {
                left -= remain;
                slot_.pending_bytes.fetch_sub(f.payload.size(), std::memory_order_relaxed);
                out_.pop_front();
            }
            else
            {
                f.offset += left;
                left = 0;
            }
        }
        if (static_cast<std::size_t>(n) < offered)
            return; // 内核缓冲满了，等下一次 EPOLLOUT
    }
    SetWriteInterest(false); // 队列排空：EPOLLOUT 仅在队列非空时注册
}

void Connection::SetWriteInterest(bool want)
{
    if (want == want_write_ || !registered_)
        return;
    want_write_ = want;
    loop_.Mod(this, fd_.get(), EPOLLIN | EPOLLRDHUP | (want ? EPOLLOUT : 0));
}

// ---- 关闭：标记 → 本轮分发之后回调 OnClosed → 销毁 ----

void Connection::Close() { CloseWith(CloseReason::LocalClose, 0); }

void Connection::CloseWith(CloseReason reason, int sys_errno)
{
    if (state_ == State::Disconnected)
        return;
    state_ = State::Disconnected;
    slot_.state.store(ConnectionTable::State::Disconnected, std::memory_order_release); // 跨线程 Send 从此得到 Closed
    close_reason_ = reason;
    close_errno_ = sys_errno;
    if (registered_)
    {
        loop_.Del(fd_.get());
        registered_ = false;
    }
    // fd 此刻仍然开着：回调栈里的代码可能还在用本对象，关 fd 与销毁都留到回收时。
    loop_.QueueClose(this);
}

void Connection::NotifyClosed()
{
    if (notified_connected_)
        handler_.OnClosed(*this, close_reason_, close_errno_);
}

} // namespace breeze
