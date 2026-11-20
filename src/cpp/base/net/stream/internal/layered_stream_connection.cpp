#include "base/net/stream/internal/layered_stream_connection.h"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

#include "base/logger/logger.h"
#include "base/logger/logger_macros.h"
#include "base/net/loop/event_loop.h"
#include "base/net/stream/internal/stream_socket.h"

namespace breeze
{

namespace
{

// 一次 read 最多收的字节数，与 PlainStreamConnection 一致：每次就绪只 read 一次，一个灌包连接不会饿死同 loop 的其他连接。
constexpr std::size_t kReadBytes = 64 * 1024;
// 每连接的缓冲只存"半个帧"，读空后若已长到这么大就释放（同 PlainStreamConnection）。
constexpr std::size_t kShrinkAbove = 4 * 1024;
// 层里待发字节的预算：到这个量就不再往层里加密，等写出去（密文缓冲因此有界，约 64KB / 连接，不计入 pending_bytes）。
constexpr std::size_t kWireBudget = 64 * 1024;

// 把一帧还没处理的部分填进 iov，返回填了几个（同 PlainStreamConnection 里的 FillIov）。
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

LayeredStreamConnection::LayeredStreamConnection(EventLoop& loop,
                                                 UniqueFd fd,
                                                 PeerInfo peer,
                                                 IConnectionHandler& handler,
                                                 std::unique_ptr<IFrameCodec> codec,
                                                 const ConnectionOptions& options,
                                                 ConnectionTable::Slot& slot,
                                                 std::uint32_t slot_index,
                                                 ConnectionHandle handle,
                                                 std::unique_ptr<IStreamLayer> layer,
                                                 bool encodes_payload)
    : StreamConnection(loop, std::move(fd), std::move(peer), handler, std::move(codec), options, slot, slot_index, handle)
    , layer_(std::move(layer))
    , encodes_payload_(encodes_payload)
{
    Set(Flag::LayerHandshaking); // 层的握手在 Establish → OnEstablish 里开始，完成时由 HandleLayerStatus 清掉这一位
}

// ---- 事件分发：与 PlainStreamConnection 相同，HUP（无 IN）→ ERR → IN/RDHUP → OUT ----

void LayeredStreamConnection::OnEvents(std::uint32_t events)
{
    if (state_ == State::Disconnected)
        return;
    if ((events & EPOLLHUP) && !(events & EPOLLIN))
    {
        CloseWith(PeerCloseReason(), 0);
        return;
    }
    if (events & EPOLLERR)
    {
        const int err = sock::GetSocketError(fd_.get());
        CloseWith(CloseReason::IoError, err != 0 ? err : EIO);
        return;
    }
    if (events & (EPOLLIN | EPOLLRDHUP))
        HandleRead();
    if (state_ != State::Disconnected && (events & EPOLLOUT))
        HandleWrite();
}

void LayeredStreamConnection::OnEstablish()
{
    HandleLayerStatus(layer_->Start());
    FlushWire();
}

void LayeredStreamConnection::HandleLayerStatus(LayerStatus st)
{
    switch (st)
    {
        case LayerStatus::Ok:
            return;
        case LayerStatus::HandshakeDone:
            HandshakeSourceDone(Flag::LayerHandshaking);
            // 握手期间 codec 的 SendRaw 先排进了 out_（那时不能加密）：层就绪了，把它们写出去。
            if (state_ != State::Disconnected && !out_.empty())
                HandleWrite();
            return;
        case LayerStatus::PeerShutdown:
            // 对端发来了关闭通知（TLS 的 close_notify）：按 RFC 回一个自己的再关，对端据此知道是正常结束而不是被截断；
            // 之后与读到 EOF 同样处理。
            (void)layer_->Shutdown();
            FlushWireBestEffort();
            CloseWith(PeerCloseReason(), 0);
            return;
        case LayerStatus::Error:
            LOG_DEBUG("stream layer error {} code={}", handle_.ToString(), layer_->error_code());
            // 层出错时往往已经在发送缓冲里放了 TLS 的致命 alert：尽力写出去再关，让对端知道为什么被拒，而不是只看到 EOF
            // （对端据此给出 TlsError 而不是 PeerClosed，日志里也有原因）。
            FlushWireBestEffort();
            CloseWith(CloseReason::TlsError, 0); // 层的底层错误码只进日志，不放进 sys_errno
            return;
    }
}

// 关闭之前尽力把层里待发的字节写出去一次（TLS 的 alert / close_notify）：不排队、不等待、不因写失败再关一次。
void LayeredStreamConnection::FlushWireBestEffort()
{
    while (state_ != State::Disconnected)
    {
        const std::span<const std::byte> wire = layer_->PendingWire();
        if (wire.empty())
            return;
        const ssize_t n = ::send(fd_.get(), wire.data(), wire.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n <= 0)
            return;
        LoopStats::Bump(loop_.stats().bytes_out, static_cast<std::uint64_t>(n));
        layer_->ConsumeWire(static_cast<std::size_t>(n));
    }
}

// ---- 读：read → layer.Unwrap（握手 / 解密）→ 分帧 ----

void LayeredStreamConnection::OnPlainChunk(std::span<std::byte> fresh)
{
    if (in_.ReadableBytes() == 0)
    {
        // 没有遗留的半帧：直接从层交来的明文分帧，只有尾部的半个帧才拷进每连接的缓冲。
        const std::size_t used = DecodeAll(fresh);
        if (used < fresh.size() && state_ != State::Disconnected)
            in_.Append(fresh.subspan(used));
        return;
    }
    in_.Append(fresh);
    in_.Consume(DecodeAll(in_.ReadableMut()));
    if (in_.ReadableBytes() == 0 && in_.Capacity() > kShrinkAbove)
        in_.ShrinkIfEmpty(0);
}

void LayeredStreamConnection::HandleRead()
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
        CloseWith(PeerCloseReason(), 0); // 读到 EOF
        return;
    }

    LoopStats::Bump(loop_.stats().bytes_in, static_cast<std::uint64_t>(n)); // 线上字节
    if (idle_timeout_ms_ != 0)
        loop_.TouchIdle(slot_index_);

    const LayerStatus st = layer_->Unwrap(
        std::span<std::byte>(buf, static_cast<std::size_t>(n)),
        [this](std::span<std::byte> plain)
        {
            if (state_ == State::Disconnected)
                return false;
            // 层只在握手完成之后才交付明文，所以"交付明文"本身就是握手完成的信号。TLS 的客户端发完 Finished 可以立刻发
            // 应用数据，两者常在同一次 read 里到达：层先交付明文、Unwrap 返回时才报 HandshakeDone。不在这里先处理的话，
            // 业务会先收到消息、后收到 OnConnected。HandshakeDone 随后再报是幂等的。
            if (Has(Flag::LayerHandshaking))
            {
                HandleLayerStatus(LayerStatus::HandshakeDone);
                if (state_ == State::Disconnected)
                    return false;
            }
            OnPlainChunk(plain);
            return state_ != State::Disconnected;
        });
    HandleLayerStatus(st);
    if (state_ != State::Disconnected)
        FlushWire(); // 握手字节、TLS 的 NewSessionTicket 之类
}

// ---- 写 ----

SendResult LayeredStreamConnection::Send(SharedBuffer payload) { return SendImpl(payload.bytes(), &payload, false); }

SendResult LayeredStreamConnection::Send(std::span<const std::byte> payload) { return SendImpl(payload, nullptr, false); }

void LayeredStreamConnection::SendFromTask(SharedBuffer payload)
{
    if (state_ != State::Connected)
        return; // 连接已标记关闭：槽回收时 pending_bytes 会清零，这里不用减
    (void)SendImpl(payload.bytes(), &payload, true);
}

// 批内相邻的 Send（EventLoop::RunSendRun）：无积压时把这一段**整段加密进层、最后只 send 一次**——K 次 send 变 1 次，
// SSL_write 的次数不变。语义与逐条 SendFromTask 一致：层内字节有预算（kWireBudget），加密不完的帧按序入队（pending_bytes
// 发送方已经加过、不再查水位）；整帧加密完的减 pending_bytes 并计 messages_out。
// 有积压（要保序）、只有一条、帧头编码失败、codec 要改写出站 payload（WebSocket 客户端的掩码要逐条拷贝）都退回逐条。
void LayeredStreamConnection::SendBatchFromTask(std::span<SharedBuffer> payloads)
{
    if (state_ != State::Connected)
        return;
    const std::size_t count = payloads.size();
    const auto one_by_one = [&]
    {
        for (SharedBuffer& p : payloads)
            SendFromTask(std::move(p));
    };
    if (count < 2 || count > kMaxSendBatch || encodes_payload_ || !out_.empty() || !layer_->PendingWire().empty())
        return one_by_one();

    loop_.AssertInLoopThread();
    std::array<std::byte, kMaxFrameHeaderSize> headers[kMaxSendBatch]; // 不清零：只有 header_len 个字节会被用到
    std::size_t header_len[kMaxSendBatch];
    for (std::size_t i = 0; i < count; ++i)
    {
        header_len[i] = codec_->EncodeHeader(payloads[i].size(), headers[i]);
        if (header_len[i] == 0)
            return one_by_one(); // 有 payload 超过 codec 上限：还没加密任何字节，交给逐条路径按原有规则关连接
    }

    std::size_t i = 0;
    std::size_t consumed = 0; // 第 i 帧已经加密的字节（帧头 + payload 合计）
    for (; i < count; ++i)
    {
        consumed = EncryptFrame(headers[i].data(), header_len[i], payloads[i].bytes(), 0);
        if (state_ == State::Disconnected)
            return; // 层出错，连接已在 EncryptFrame 里关闭
        if (consumed != header_len[i] + payloads[i].size())
            break; // 层内字节到预算了：这一帧（及之后）剩下的入队
        slot_.pending_bytes.fetch_sub(payloads[i].size(), std::memory_order_relaxed);
        LoopStats::Bump(loop_.stats().messages_out);
    }
    for (; i < count; ++i) // 第一帧带已加密的偏移，其后整帧入队，保序
    {
        Pending& p = out_.emplace_back();
        p.header = headers[i];
        p.header_len = static_cast<std::uint8_t>(header_len[i]);
        p.payload = std::move(payloads[i]);
        p.offset = consumed;
        consumed = 0;
        LoopStats::Bump(loop_.stats().messages_out);
    }
    FlushWire(); // 整段一次 send；写不完它自己注册 EPOLLOUT
    if (state_ != State::Disconnected && !out_.empty() && !want_write_)
        SetWriteInterest(true);
}

std::size_t LayeredStreamConnection::EncryptFrame(const std::byte* header,
                                                  std::size_t header_len,
                                                  std::span<const std::byte> payload,
                                                  std::size_t offset)
{
    // 第三块线程本地缓冲（读密文、层里的读明文之外）：只给"帧头 + payload 开头"拼成一个 record 用。
    // OnMessage 里重入 Send 时，读明文缓冲正被业务引用，所以写路径不能复用它。
    static thread_local std::byte stage[kLayerMaxWrapBytes];

    const std::size_t total = header_len + payload.size();
    std::size_t done = offset;
    while (done < total && layer_->PendingWire().size() < kWireBudget)
    {
        std::span<const std::byte> chunk;
        if (done < header_len)
        {
            // 帧头还没发完：和 payload 开头拼进暂存，一个 record 而不是两个
            const std::size_t hrem = header_len - done;
            std::memcpy(stage, header + done, hrem);
            const std::size_t take = std::min(kLayerMaxWrapBytes - hrem, payload.size());
            if (take != 0)
                std::memcpy(stage + hrem, payload.data(), take);
            chunk = {stage, hrem + take};
        }
        else
        {
            // 其后的 record 直接从 payload 内存加密，不经暂存
            const std::size_t poff = done - header_len;
            chunk = payload.subspan(poff, std::min(kLayerMaxWrapBytes, payload.size() - poff));
        }
        if (layer_->Wrap(chunk) == LayerStatus::Error)
        {
            HandleLayerStatus(LayerStatus::Error);
            return done;
        }
        done += chunk.size();
    }
    return done;
}

void LayeredStreamConnection::EncryptQueued()
{
    static thread_local std::byte stage[kLayerMaxWrapBytes];

    while (!out_.empty() && layer_->PendingWire().size() < kWireBudget)
    {
        // 队首帧的帧头已发完、剩下的 payload 够一个 record：直接从 payload 内存加密，不经暂存
        Pending& front = out_.front();
        std::span<const std::byte> chunk;
        if (front.offset >= front.header_len && front.Total() - front.offset >= kLayerMaxWrapBytes)
        {
            const std::size_t poff = front.offset - front.header_len;
            chunk = front.payload.bytes().subspan(poff, kLayerMaxWrapBytes);
        }
        else
        {
            // 否则把队首起若干帧的剩余部分拼进暂存，至多一个 record：积压里的小帧合并成一个 record，是免费的写合并
            std::size_t n = 0;
            for (auto it = out_.begin(); it != out_.end() && n < kLayerMaxWrapBytes; ++it)
            {
                iovec iov[2];
                const int cnt = FillIov(it->header.data(), it->header_len, it->payload.bytes(), it->offset, iov);
                bool full = false;
                for (int i = 0; i < cnt; ++i)
                {
                    const std::size_t take = std::min(iov[i].iov_len, kLayerMaxWrapBytes - n);
                    std::memcpy(stage + n, iov[i].iov_base, take);
                    n += take;
                    if (take < iov[i].iov_len)
                    {
                        full = true;
                        break;
                    }
                }
                if (full)
                    break;
            }
            chunk = {stage, n};
        }

        if (layer_->Wrap(chunk) == LayerStatus::Error)
        {
            HandleLayerStatus(LayerStatus::Error);
            return;
        }

        // 推进：整帧加密完的弹出并从 pending_bytes 里减去它的 payload，没完的记偏移
        std::size_t left = chunk.size();
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
    }
}

void LayeredStreamConnection::FlushWire()
{
    while (state_ != State::Disconnected)
    {
        const std::span<const std::byte> wire = layer_->PendingWire();
        if (wire.empty())
            return;
        const ssize_t n = ::send(fd_.get(), wire.data(), wire.size(), MSG_NOSIGNAL);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                SetWriteInterest(true); // 内核缓冲满了，等 EPOLLOUT
                return;
            }
            LoopStats::Add(loop_.stats().send_closed_io_error);
            CloseWith(CloseReason::IoError, errno);
            return;
        }
        LoopStats::Bump(loop_.stats().bytes_out, static_cast<std::uint64_t>(n));
        layer_->ConsumeWire(static_cast<std::size_t>(n));
    }
}

// owned 非空：调用方交来的 SharedBuffer，入队时直接接管；为空：payload 只是个视图，入队时才拷贝。
SendResult LayeredStreamConnection::SendImpl(std::span<const std::byte> payload, SharedBuffer* owned, bool counted)
{
    loop_.AssertInLoopThread();
    if (state_ != State::Connected)
    {
        LoopStats::Add(loop_.stats().send_closed_stale);
        return SendResult::Closed;
    }

    std::array<std::byte, kMaxFrameHeaderSize> header;
    const std::size_t header_len = codec_->EncodeHeader(payload.size(), header);
    if (header_len == 0)
    {
        LOG_ERROR("Send 的 payload 超过 codec 上限 size={}，关闭连接", payload.size());
        LoopStats::Add(loop_.stats().send_closed_oversize);
        CloseWith(CloseReason::ProtocolError, 0);
        return SendResult::Closed;
    }

    // codec 要改写出站 payload（WebSocket 客户端的掩码）：拷一份、在拷贝上改写，之后用这一份。
    SharedBuffer encoded;
    if (encodes_payload_)
    {
        encoded = SharedBuffer::CopyFrom(payload);
        codec_->EncodePayload(encoded.MutableBytes());
        payload = encoded.bytes();
        owned = &encoded;
    }

    std::size_t consumed = 0; // 已经加密的字节（帧头 + payload 合计）
    if (out_.empty() && layer_->PendingWire().empty())
    {
        // 无积压：直写。明文被加密吃掉之后，写不完的是密文、留在层里，不需要把 payload 拷成 SharedBuffer。
        consumed = EncryptFrame(header.data(), header_len, payload, 0);
        if (state_ == State::Disconnected)
            return SendResult::Closed;
        FlushWire();
        if (state_ == State::Disconnected)
            return SendResult::Closed;
        if (consumed == header_len + payload.size())
        {
            if (counted)
                slot_.pending_bytes.fetch_sub(payload.size(), std::memory_order_relaxed);
            LoopStats::Bump(loop_.stats().messages_out);
            return SendResult::Ok;
        }
    }

    // 已有积压（排在后面保序），或本帧超出预算没加密完：剩余明文入队，靠 EPOLLOUT 排空。
    if (!counted)
    {
        const std::uint64_t pending =
            slot_.pending_bytes.fetch_add(payload.size(), std::memory_order_relaxed) + payload.size();
        if (pending > high_water_)
        {
            LOG_WARN("发送积压 {} 字节超过水位 {}，按慢消费者关闭连接", pending, high_water_);
            LoopStats::Add(loop_.stats().send_closed_slow_consumer);
            CloseWith(CloseReason::SlowConsumer, 0);
            return SendResult::Closed;
        }
    }
    Pending& p = out_.emplace_back();
    p.header = header;
    p.header_len = static_cast<std::uint8_t>(header_len);
    p.payload = owned != nullptr ? std::move(*owned) : SharedBuffer::CopyFrom(payload);
    p.offset = consumed;
    if (!want_write_)
        SetWriteInterest(true);
    LoopStats::Bump(loop_.stats().messages_out);
    return SendResult::Ok;
}

void LayeredStreamConnection::WriteRaw(std::span<const std::byte> bytes)
{
    loop_.AssertInLoopThread();
    if (state_ == State::Disconnected || bytes.empty())
        return;

    std::size_t consumed = 0;
    // 握手期间（层还没就绪）不能加密：先排进 out_，层就绪后由 HandleLayerStatus 写出去。
    if (!Has(Flag::LayerHandshaking) && out_.empty() && layer_->PendingWire().empty())
    {
        consumed = EncryptFrame(nullptr, 0, bytes, 0);
        if (state_ == State::Disconnected)
            return;
        FlushWire();
        if (state_ == State::Disconnected || consumed == bytes.size())
            return;
    }

    const std::uint64_t pending = slot_.pending_bytes.fetch_add(bytes.size(), std::memory_order_relaxed) + bytes.size();
    if (pending > high_water_)
    {
        LOG_WARN("发送积压 {} 字节超过水位 {}，按慢消费者关闭连接", pending, high_water_);
        LoopStats::Add(loop_.stats().send_closed_slow_consumer);
        CloseWith(CloseReason::SlowConsumer, 0);
        return;
    }
    Pending& p = out_.emplace_back();
    p.header_len = 0;
    p.payload = SharedBuffer::CopyFrom(bytes);
    p.offset = consumed;
    if (!Has(Flag::LayerHandshaking) && !want_write_)
        SetWriteInterest(true);
}

void LayeredStreamConnection::HandleWrite()
{
    for (;;)
    {
        FlushWire();
        if (state_ == State::Disconnected)
            return;
        if (!layer_->PendingWire().empty())
            return; // 内核缓冲满了，等下一次 EPOLLOUT
        if (out_.empty() || Has(Flag::LayerHandshaking))
            break; // 握手期排队的 codec 原始字节，等层就绪再加密
        EncryptQueued();
        if (state_ == State::Disconnected)
            return;
    }
    SetWriteInterest(false); // 队列与层里都排空：EPOLLOUT 仅在有东西要写时注册
    if (state_ == State::Disconnecting)
        FinishWrites(); // 优雅关闭：该发的都发完了，让层发关闭通知、再告诉对端我们这边结束
}

void LayeredStreamConnection::FinishWrites()
{
    // 写队列已排空：让层发关闭通知（TLS 的 close_notify），排空之后才 shutdown(SHUT_WR)。幂等：排空不了会再来一次。
    if (!layer_shutdown_)
    {
        layer_shutdown_ = true;
        (void)layer_->Shutdown();
    }
    FlushWire();
    if (state_ != State::Disconnected && layer_->PendingWire().empty())
        ShutdownWrite();
}

} // namespace breeze
