// Agent 一侧：监听一个 UDS，对每个来握手的 App 创建一条共享内存通道，把 memfd 与两个 eventfd 交给它，
// 并替调用方跑完 Tx 环的消费循环（唤醒协议）。设计见 docs/共享内存IPC设计.md §5 / §6.1。
//
// 用法与 StreamServer 一致：收 EventLoopGroup&，Start() 阻塞到 bind / listen 在 IO 线程上完成，
// 停机走 group 的停机钩子。
// 一个服务端的全部通道、握手与回调都在 accept 所在的那一个 loop 上
// （第一版容量上限就是一个 IO 线程；分到多个 loop 以后再说）。
//
// 回调全部在那个 loop 的 IO 线程上，必须非阻塞、noexcept。
//
// 通道的生命：App 的 UDS 断开不销毁通道，而是进入 Detached（保留）：
//   Attached ──UDS 断开──▶ Detached ──超过保留时间──▶ 销毁（OnClosed(RetentionExpired)）
//       ▲                     │
//       └─ 同一 busid 的 Attach（新进程）/ Reattach（原进程）─┘
// Detached 期间 Tx 里已提交的记录照常经 OnReadable 排空（遗言），rx() 仍可写（攒着，环满了是 Full）。
// App 正常退出发 Bye：排空 Tx 之后立刻销毁（OnClosed(PeerBye)），不保留；PrepareRestart 设置这一次的保留时长。
// Agent 重启：App 用 Reattach 把手里的 fd 传回来，OnOpened 里 reattached() 为真；上一任 Agent 死在处理一条记录的期间时，
// 那一条会被跳过（既不重发，也不会把新 Agent 拖进崩溃循环，见 shm_ring.h 的 RecoverFromCrash）。
// **handler 里的映射要以 ShmServerChannel::id() 为键，不要只用 busid**：discard_retained 重建时，
// 同名的新通道会先于旧通道的 OnClosed 出现。
//
// 生命周期：handler 只持引用，调用方保证它比 EventLoopGroup::Shutdown 活得久（Shutdown 之后不再有回调）；
// 本对象自己也必须覆盖到 Shutdown 返回之后（loop 上可能仍有引用 this 的任务）。

#pragma once

#include <sys/socket.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "base/net/codec/frame_codec_interface.h"
#include "base/net/connection/connection_context.h"
#include "base/net/connection/connection_handler_interface.h"
#include "base/net/connection/connection_options.h"
#include "base/net/endpoint/endpoint.h"
#include "base/net/endpoint/peer_info.h"
#include "base/net/loop/event_loop_group.h"
#include "base/net/shm/shm_protocol.h"
#include "base/shm/shm_channel.h"
#include "base/shm/shm_defs.h"
#include "base/shm/shm_ring.h"

namespace breeze
{

enum class ShmCloseReason : std::uint8_t
{
    PeerClosed,       // UDS 断了而且不保留（retain_default 为 0），Tx 已排空
    PeerBye,          // App 发了 Bye 然后退出，Tx 已排空（或排空超时）
    RetentionExpired, // Detached 超过保留时间没人回来：队列作废
    Discarded,        // 同名 Attach 带 discard_retained：旧队列被丢弃，按新容量重建
    LocalClose,       // 回调里调了 Close()
    ProtocolError,    // 握手之后对端往 UDS 里写了不合法的东西
    IoError,          // UDS 出错
    ServerStopped,    // 服务端停止（Stop / EventLoopGroup::Shutdown）
    HandshakeTimeout, // 握手超时（只在内部计数，没有 OnClosed：通道从未建立）
};

constexpr const char* ToString(ShmCloseReason r) noexcept
{
    switch (r)
    {
        case ShmCloseReason::PeerClosed:
            return "PeerClosed";
        case ShmCloseReason::PeerBye:
            return "PeerBye";
        case ShmCloseReason::RetentionExpired:
            return "RetentionExpired";
        case ShmCloseReason::Discarded:
            return "Discarded";
        case ShmCloseReason::LocalClose:
            return "LocalClose";
        case ShmCloseReason::ProtocolError:
            return "ProtocolError";
        case ShmCloseReason::IoError:
            return "IoError";
        case ShmCloseReason::ServerStopped:
            return "ServerStopped";
        case ShmCloseReason::HandshakeTimeout:
            return "HandshakeTimeout";
    }
    return "?";
}

// 一条已经建立的通道，Agent 一侧的视图。只能在 IO 线程使用；从 OnOpened 开始、到 OnClosed 返回为止有效。
class ShmServerChannel
{
public:
    virtual ~ShmServerChannel() = default;

    // 通道的身份：从建立到销毁不变（保留、恢复都不换）。
    virtual std::uint64_t id() const noexcept = 0;
    virtual const std::string& busid() const noexcept = 0;
    // SO_PEERCRED：连上来的那个进程的身份（握手时取的）。
    virtual const PeerCredentials& peer() const noexcept = 0;
    virtual shm::ShmChannel& shm() noexcept = 0;
    // Tx 环（App → Agent）的消费者、Rx 环（Agent → App）的生产者。库已经替你建好，不要再从 shm() 另取一份。
    virtual shm::RingConsumer& tx() noexcept = 0;
    virtual shm::RingProducer& rx() noexcept = 0;
    virtual std::uint64_t app_epoch() const noexcept = 0;
    virtual std::uint64_t agent_epoch() const noexcept = 0;
    // 此刻 App 不在（UDS 断了，队列保留中）。
    virtual bool detached() const noexcept = 0;
    // 这条通道是 Agent 重启之后 App 用 Reattach 带回来的：里面可能有积压的 Tx 记录，Rx 里可能有 App 还没读的。
    virtual bool reattached() const noexcept = 0;

    // 请求销毁（比如 tx().Peek 返回 kCorrupt），不走保留。可以在回调里调；OnClosed(LocalClose) 随后在同一个 IO 线程上回调，
    // 不会重入当前回调。重复调用是空操作。
    virtual void Close() noexcept = 0;
};

struct ShmAdmitRequest
{
    std::string_view busid;
    PeerCredentials peer;
    std::int32_t claimed_pid;  // 对端在 Hello 里自称的，只供日志
    std::uint64_t tx_capacity; // Hello 里的值，0 = 要默认
    std::uint64_t rx_capacity;
    shm::wire::Transport transport = shm::wire::Transport::kShmRequired; // Hello 里 App 想用的传输方式
    bool reattach = false;          // Agent 重启之后 App 带着旧 fd 来恢复
    bool resuming = false;          // 这次会接手一条保留着的通道（App 重启，或 Reattach 命中了还留着的那条）
    bool discard_retained = false;  // Attach 带了 discard_retained：会丢弃保留着的队列
};

class ShmChannelHandler
{
public:
    virtual ~ShmChannelHandler() = default;

    // 准入：身份（uid 白名单）、通道名策略。返回 0 放行；
    // 否则返回 errno（比如 EACCES），原样作为 Reject 的原因发给对端。
    // 在创建共享内存之前调用，拒绝的代价只是一次 Reply。
    virtual int OnAdmit(const ShmAdmitRequest&) noexcept { return 0; }

    // 通道已建立（Accept 已经发出）。reattached() 为真时是 Agent 重启之后恢复的旧通道。
    virtual void OnOpened(ShmServerChannel&) noexcept {}

    // App 的 UDS 断了，队列保留 retain 这么久（默认 retain_default，App 退出前可经 PrepareRestart 改）。
    // 之后要么 OnResumed，要么 OnClosed(RetentionExpired)。Bye / retain_default 为 0 时不经过这里，直接排空并销毁。
    virtual void OnDetached(ShmServerChannel&, std::chrono::milliseconds /*retain*/) noexcept {}

    // 同一 busid 的 App 回来了：新进程 Attach 恢复（app_epoch 已加一），或原进程在 Agent 没重启时 Reattach。
    // 队列原样接着用；Detached 期间积压在 Rx 里的消息 App 马上会读到。
    virtual void OnResumed(ShmServerChannel&) noexcept {}

    // Tx 环里有记录：在预算内 Peek / Pop，处理完。**不要自己调 WokeUp / PrepareToSleep，库替你做**。
    // 返回 true = 预算用完、环里可能还有（库在下一轮事件循环再调一次，别的通道不会被饿死）；
    // 返回 false = 搬空了（库随后做睡前检查，期间又来了记录会马上再调一次）。
    // tx().Peek 返回 kCorrupt 时应当 Close()。
    virtual bool OnReadable(ShmServerChannel&) noexcept = 0;

    // 通道销毁。每个回调过 OnOpened 的通道恰触发一次，且在其最后一个 OnReadable 之后；此后不再有该通道的任何回调。
    virtual void OnClosed(ShmServerChannel&, ShmCloseReason) noexcept {}
};

// 仅 UDS 的对端（Hello 要 UdsOnly，或要 ShmPreferred 而 Agent 给不了共享内存、退化过来的）：握手之后这条 UDS 交给
// StreamConnection 当数据通道（帧由 factory 创建的 IFrameCodec 切），消息经 handler 的 OnMessage 到达。
// 业务回调里 `IConnection::GetContext<UdsPeerContext>()` 取到这条连接的身份（busid / SO_PEERCRED）。
// 没有提供 handler / factory 时，仅 UDS 的 Hello 回 Reject(ENOTSUP)，ShmPreferred 在共享内存给不了时回 Reject。
// handler 与 factory 只持引用，要比这些连接活得久（同 StreamServer 的约定）。
struct ShmUdsDataPath
{
    IConnectionHandler* handler = nullptr;
    const IFrameCodecFactory* factory = nullptr;
    ConnectionOptions connection;
};

struct UdsPeerContext final : ConnectionContext
{
    std::string busid;
    PeerCredentials cred;
    std::uint64_t channel_id = 0;
    std::shared_ptr<void> keepalive; // 库内部用：让连接的中间处理器活得比连接久，业务不要碰
};

struct ShmServerOptions
{
    // 单环容量上限；Hello 要的比它大就 Reject(EINVAL)。
    std::uint64_t max_ring_capacity = shm::kDefaultMaxRingCapacity;
    // 同时存在的通道数上限，超过 Reject(ENOSPC)。
    std::size_t max_channels = 1024;
    // 所有通道的共享内存总字节数上限（按 memfd 大小算），超过 Reject(ENOSPC)。
    std::uint64_t max_total_bytes = 4ULL << 30;
    // 已连上、还没握手完的连接数上限；超过的直接断开（防止连接不发 Hello 占满 fd）。
    std::size_t max_pending_handshakes = 256;
    // 握手超时：连上之后这么久没收齐 Hello 就断开。
    std::chrono::milliseconds handshake_timeout{5000};
    // App 的 UDS 断开之后队列保留多久（App 没发 PrepareRestart / Bye 时）；0 = 不保留，排空 Tx 就销毁。
    std::chrono::milliseconds retain_default{30000};
    // PrepareRestart 能要的保留时长上限（超过的按上限算）。
    std::chrono::milliseconds retain_max{600000};
    // 不保留时（Bye / retain_default 为 0）等 Tx 排空的上限；到了就直接销毁。
    std::chrono::milliseconds drain_timeout{5000};
    int backlog = SOMAXCONN;
    // 是否提供共享内存。false 时：要 ShmPreferred 的退化成仅 UDS（要有 ShmUdsDataPath），要 ShmRequired 的 Reject(ENOTSUP)。
    bool allow_shm = true;
};

// 各计数是某一时刻的快照，任意线程可读。
struct ShmServerStats
{
    std::uint64_t opened = 0;             // 建立过的通道总数
    std::uint64_t closed = 0;             // 已关闭的
    std::uint64_t open_channels = 0;      // 此刻存在的
    std::uint64_t rejected = 0;           // 握手被拒（任何原因，已回 Reject）
    std::uint64_t handshake_timeouts = 0; // 握手超时被断开
    std::uint64_t pending_overflow = 0;   // 因 max_pending_handshakes 直接断开
    std::uint64_t protocol_errors = 0;    // 握手之后对端往 UDS 里写了不合法的东西
    std::uint64_t detached_channels = 0;  // 此刻处于保留（Detached）的通道数
    std::uint64_t detached_total = 0;     // 进入过保留的次数
    std::uint64_t resumed = 0;            // Attach 恢复保留着的通道的次数
    std::uint64_t reattached = 0;         // Reattach 成功的次数（含命中还留着的那条）
    std::uint64_t retention_expired = 0;  // 保留到期被销毁的通道数
    std::uint64_t discarded = 0;          // 被 discard_retained 丢弃的通道数
    std::uint64_t byes = 0;               // 收到 Bye 的次数
    std::uint64_t inflight_skipped = 0;   // Reattach 时跳过的"上一任 Agent 处理到一半"的记录数
    std::uint64_t rx_bytes_discarded = 0; // 保留到期时 Rx 环里没被读走的字节数（环里的占用，含对齐补白）
    std::uint64_t uds_channels = 0;       // 此刻存在的仅 UDS 通道数（open_channels 含它）
    std::uint64_t uds_total = 0;          // 建立过的仅 UDS 通道总数（opened 含它）
    std::uint64_t downgraded = 0;         // ShmPreferred 因共享内存给不了而退化成仅 UDS 的次数（uds_total 含它）
};

class ShmChannelServer
{
public:
    // uds：仅 UDS 的数据通道（见 ShmUdsDataPath）；不给就只支持共享内存。
    ShmChannelServer(EventLoopGroup& group,
                     const Endpoint& endpoint,
                     ShmServerOptions options,
                     ShmChannelHandler& handler,
                     ShmUdsDataPath uds = {});
    ~ShmChannelServer();

    ShmChannelServer(const ShmChannelServer&) = delete;
    ShmChannelServer& operator=(const ShmChannelServer&) = delete;

    // group 必须已 Start（否则是编程错误，LOG_FATAL）；只能调用一次。阻塞到 bind / listen 在 IO 线程上完成。
    // 0 = 成功，否则 errno（EADDRINUSE、EACCES、ENAMETOOLONG……），失败时已回滚；group 正在 / 已经停机返回 ESHUTDOWN；
    // 端点不是 UDS（Unix / UnixAbstract）返回 EPROTONOSUPPORT：fd 只能在 UDS 上传。
    [[nodiscard]] int Start();

    // 停止接受新连接，并关闭全部通道（OnClosed(ServerStopped)）。异步；对没 Start 过的对象是空操作。
    void Stop();

    const Endpoint& endpoint() const noexcept { return endpoint_; }
    ShmServerStats stats() const noexcept;

    // 实现细节（定义在 .cpp）。声明放在公开区只是为了让 .cpp 里的辅助类能引用它，使用方不要碰。
    struct Impl;

private:
    Endpoint endpoint_;
    std::unique_ptr<Impl> impl_;
};

} // namespace breeze
