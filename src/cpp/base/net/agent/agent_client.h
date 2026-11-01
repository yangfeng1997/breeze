// App 一侧连本机 Agent 的客户端：握手时协商传输方式——共享内存（+ UDS 探活与控制）或仅 UDS——然后表现成一条普通的 IConnection，
// 业务用 IConnectionHandler 收事件、经 ConnectionHandle / INetClient::Send 发消息，不用关心底下是哪种传输。
// 设计见 docs/共享内存IPC设计.md §6.1 / §6.2 / §6.5。
//
// 传输方式（AgentClientOptions::transport，App 侧可配置）：
//   kUdsOnly       不启用共享内存：握手之后这条 UDS 就是数据通道（StreamConnection，帧由 codec 切）。
//   kShmPreferred  要共享内存；Agent 不提供或给不了（内存配额满）时，Agent 会让它退化成仅 UDS，本客户端照单全收。（默认）
//   kShmRequired   要共享内存；给不了就失败（on_connect_failed 报 ENOTSUP / ENOSPC 等）。
// 连上之后 link() 告诉你实际走的是哪一种。
//
// 共享内存模式下的生命周期（与普通客户端不同的地方）：
// - **Agent 掉了连接不关**：UDS 断开（Agent 退出或被 kill）只置 agent_down()，业务的 Send 照常写 Tx 环、Rx 里剩的照常读完；
//   后台按退避重连并 Reattach（把手里的 fd 传回新 Agent，队列原样接着用）。Reattach 被判定不可能成功（内存校验不过、
//   被别的进程接管）才关闭这条连接，再走一遍全新的 Attach。
// - **App 重启**：新进程用同一个 busid 连，Agent 在保留期内把原来的通道还给它（resumed），积压的消息一连上就交给 OnMessage。
//   退出前 PrepareRestart(ms) 要更长的保留；正常退出 Stop()（会发 Bye：Agent 排空 Tx 就销毁，不保留）。
// - 环满了这条丢掉：Send 返回 SendResult::Full（连接还开着，可稍后重试），并计入 stats().tx_full_drops；
//   跨线程 Send 的写环是异步的，环满之后约 ShmConnection::kFullHintWindow 内的后续发送当场得到 Full，
//   提示置位之前已投递的几条仍可能返回 Ok 却被丢（只进 tx_full_drops）：Full 保证没发出，Ok 不保证发出了。
// 仅 UDS 模式：UDS 断了就是连接关闭，按 reconnect 策略重新握手（没有队列可保留）。
//
// 线程：Start / Stop / Send / PrepareRestart 任意线程；回调在所属 IO 线程。握手、探活、重连都是非阻塞的，挂在 EventLoop 上。
// handler 与 codec 只持引用，要比 EventLoopGroup::Shutdown 活得久；本对象析构时同步停掉自己并等连接回收完
// （之后不会再有这个客户端的回调）——所以**不能在它所属的 IO 线程上析构**（要同步等那个 loop，会死锁）。

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include "base/net/codec/frame_codec_interface.h"
#include "base/net/connection/connection_handler_interface.h"
#include "base/net/connection/connection_options.h"
#include "base/net/connection/net_client_interface.h"
#include "base/net/endpoint/endpoint.h"
#include "base/net/loop/event_loop_group.h"
#include "base/net/shm/shm_protocol.h"
#include "base/net/stream/reconnect_policy.h"
#include "base/shm/shm_channel.h"

namespace breeze::agent
{

struct AgentClientOptions
{
    std::string busid; // 通道名，[A-Za-z0-9._:@-]{1,63}，同一个 Agent 上唯一（NodeId 的十进制写法也行，见 AgentRouter）
    shm::wire::Transport transport = shm::wire::Transport::kShmPreferred;
    // 环的容量与单条上限：0 = 默认；其余必须是 2 的幂且在 [1 MiB, Agent 的单环上限]（共享内存模式用）。
    std::uint64_t tx_capacity = 0;
    std::uint64_t rx_capacity = 0;
    std::uint32_t tx_max_payload = 0;
    std::uint32_t rx_max_payload = 0;
    // 丢弃 Agent 保留着的同名队列重建（想换环的大小、或把传输方式换成仅 UDS 时用）。
    bool discard_retained = false;
    shm::AttachLimits limits; // 接手时对 Agent 给的容量设的上限
    ConnectionOptions connection;
    // 一次握手（连接 + 发 Hello + 等 Reply）的时限。
    std::chrono::milliseconds handshake_timeout{3000};
    // 握手失败 / 连接关闭之后、以及 Agent 掉了之后 Reattach 的退避。Agent 在本机，默认比跨机的客户端勤快。
    ReconnectPolicy reconnect{std::chrono::milliseconds(100), std::chrono::milliseconds(5000), 2.0, 0.2};
    // 每次握手失败时回调（IO 线程，非阻塞、noexcept）。will_retry 为 false 表示不可重试的错误
    // （EINVAL / EACCES / ENOTSUP / EPROTO / EPROTONOSUPPORT）：状态回到 Idle，要调用方显式 Start()。
    ConnectFailedCallback on_connect_failed;
    // 每次读 Rx 环最多交给业务几条（用完让出，别的连接不被饿死）。
    std::size_t rx_budget = 64;
    // 仅 UDS（含退化）时数据通道的帧格式，必须与 Agent 那边的 ShmUdsDataPath 一致；不给就用 LengthPrefixCodec（默认上限）。
    const IFrameCodecFactory* codec = nullptr;
};

// 各计数是某一时刻的快照，任意线程可读。
struct AgentClientStats
{
    std::uint64_t attaches = 0;          // 握手成功的次数（含退化成仅 UDS 的）
    std::uint64_t attach_failures = 0;   // 握手失败的次数
    std::uint64_t reattaches = 0;        // Reattach 成功的次数
    std::uint64_t reattach_failures = 0; // Reattach 失败的次数（含重试）
    std::uint64_t agent_down_total = 0;  // 发现 Agent 掉线的次数
    std::uint64_t tx_messages = 0;       // 写进 Tx 环的消息数（所有共享内存连接累计）
    std::uint64_t tx_full_drops = 0;     // 写 Tx 环时遇满被丢的（跨线程被提示当场拒绝的不在内，见 NetStats::send_full）
    std::uint64_t tx_too_large = 0;      // 超过单条上限被丢的
    std::uint64_t rx_messages = 0;       // 从 Rx 环读出并交给业务的
    std::uint64_t last_backoff_ms = 0;   // 最近一次退避等了多久（诊断用：成功之后应当回到 reconnect.initial_delay）
};

class AgentClient final : public INetClient
{
public:
    enum class Link : std::uint8_t
    {
        kNone, // 还没连上（或已关闭）
        kShm,  // 共享内存
        kUds,  // 仅 UDS
    };

    AgentClient(EventLoopGroup& group, Endpoint endpoint, AgentClientOptions options, IConnectionHandler& handler);
    ~AgentClient() override;

    // INetClient。Start 只返回参数类错误（busid 不合法、端点不是 UDS、transport 不合法 → EINVAL / EPROTONOSUPPORT；
    // group 正在停机 ESHUTDOWN）；握手失败走 on_connect_failed 与退避重试。
    [[nodiscard]] int Start() override;
    // 停止重连并关闭当前连接（共享内存模式先发 Bye）。该连接的 OnClosed 仍会且仅会触发一次。
    void Stop() override;
    // 经当前连接发送。未连通 → Closed，不缓冲；共享内存模式 Tx 环满 → Full（没发出，见头注释）。线程安全。
    [[nodiscard]] SendResult Send(SharedBuffer payload) override;
    ConnectionHandle handle() const noexcept override;
    const Endpoint& endpoint() const noexcept override { return endpoint_; }

    // App 马上要重启（热更）：请 Agent 把队列多留 retain 这么久（只在共享内存模式、UDS 还连着时有效）。任意线程。
    // 调过之后的 Stop() / 析构当作"重启"，不再发 Bye（发了 Agent 就立刻销毁队列，保留就白要了）。
    void PrepareRestart(std::chrono::milliseconds retain);

    Link link() const noexcept;
    // 共享内存模式下 Agent 掉线、正在后台 Reattach。
    bool agent_down() const noexcept;
    AgentClientStats stats() const noexcept;

    struct Impl; // 实现细节（定义在 .cpp）

private:
    Endpoint endpoint_;
    std::unique_ptr<Impl> impl_;
};

} // namespace breeze::agent
