// Agent 的路由器：本机的 App（经共享内存或仅 UDS）与别的 Agent（经 TCP / UDS 链路）发来的每条消息，
// 解析信封、核对身份、判过期、查路由表，再投递到目的地：本机另一个 App 的 Rx 环 / 仅 UDS 连接，或通向别的 Agent 的链路。
// 信封格式见 envelope.h，路由表见 route_table.h，设计与取舍见 docs/共享内存IPC设计.md §4 / §10。
//
// 使用：同一个 AgentRouter 同时作为 ShmChannelServer 的 ShmChannelHandler（共享内存通道）和 ShmUdsDataPath 的
// IConnectionHandler（仅 UDS 的 App）；Agent 之间的链路（TcpServer / TcpClient 等）也用它当 IConnectionHandler。
// **路由器全部跑在一个 EventLoop 线程上**：ShmChannelServer 的通道与回调本来就都在 accept 所在的那个 loop 上，
// 所以 Agent 进程的 EventLoopGroup 要配 io_threads = 1（路由表与对端表都是单线程访问，不加锁）。
// 回调全部非阻塞、noexcept。
//
// 语义（都对应设计 §4 / §6）：
// - **只做转发，不改 seq**；**Request 不可达时回一条错误 Response**，让调用方快速失败而不是等满 deadline；
//   Oneway / Response 不可达、过期、环满：丢并计数，不通知发送方（单向可丢，已定）。
// - **本机 App 的 src 必须等于握手时登记的 NodeId**，否则丢弃并计 bad_src（防冒充）；来自别的 Agent 的链路是受信的，不核对。
// - **过期**：出队时用记录头里的入队时刻把排队时间扣掉再写进转发的拷贝；不是从环里来的（UDS / 链路的帧）排队时间记 0。
// - **释放策略**：共享内存通道上"先标记 inflight、转发完再 Pop"，Agent 崩溃后新 Agent 跳过处理到一半的那一条（至多一次）。
// - **Detached 的目的地**照常往它的 Rx 环里写（攒着，满了丢并计 rx_full_detached）。

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "base/net/agent/envelope.h"
#include "base/net/agent/route_table.h"
#include "base/net/connection/connection_handler_interface.h"
#include "base/net/shm/shm_channel_server.h"

namespace breeze::agent
{

// Request 无法送达时回给调用方的错误码（Response 信封的 err_code）。
inline constexpr std::uint32_t kRouteErrNoRoute = 1;      // 路由表给不出目的地
inline constexpr std::uint32_t kRouteErrUnavailable = 2;  // 目的地在，但送不进去（环满、连接已关、消息太大）

struct AgentRouterOptions
{
    // 握手时把通道名映射成 NodeId（这个 App 是谁）。返回空 = 不认识，准入拒绝（EACCES）。
    // 默认：通道名就是十进制的 NodeId，比如 "65793"。
    std::function<std::optional<NodeId>(std::string_view busid, const PeerCredentials&)> node_of;
    // 每次 OnReadable 最多处理几条（用完就让出，别的通道不会被饿死）。
    std::size_t read_budget = 64;
    // 单调时钟（纳秒），要与共享内存环盖入队时刻用的是同一个（Now::SteadyNs）。默认就是它；只给测试注入用：
    // 拨快它就能不真的等待而模拟"排了很久的队"。
    std::function<std::uint64_t()> clock;
    // 错误 Response 信封里的 remaining_ms（调用方拿到时它自己的 deadline 早该判过了，这个值只要不是 0）。
    std::uint32_t error_reply_ttl_ms = 5000;
};

// 各计数是某一时刻的快照，任意线程可读。
struct AgentRouterStats
{
    std::uint64_t received_shm = 0;    // 从共享内存环收到的（不论后来是否转发成功）
    std::uint64_t received_uds = 0;    // 从仅 UDS 的 App 收到的
    std::uint64_t received_link = 0;   // 从别的 Agent 的链路收到的
    std::uint64_t delivered_shm = 0;   // 写进本机 App 的 Rx 环的（一条消息发给多个目的地算多次）
    std::uint64_t delivered_uds = 0;   // 发给本机仅 UDS 的 App 的
    std::uint64_t delivered_link = 0;  // 转发给别的 Agent 的
    std::uint64_t bad_envelope = 0;    // 信封解析不过（任何原因）
    std::uint64_t bad_src = 0;         // 本机 App 的 src 与登记的 NodeId 不符
    std::uint64_t stale_drops = 0;     // 过期丢弃
    std::uint64_t invalid_expire = 0;  // remaining_ms 或入队时刻为 0，按过期丢弃
    std::uint64_t hops_exceeded = 0;   // 转发次数到上限（疑似环路）
    std::uint64_t no_route = 0;        // 路由表给不出目的地，或目的 Node 不认识
    std::uint64_t rx_full = 0;         // 目的地的 Rx 环满了
    std::uint64_t rx_full_detached = 0; // 同上，目的地此刻在保留中（App 不在）
    std::uint64_t too_large = 0;       // 消息超过目的地环的单条上限
    std::uint64_t send_closed = 0;     // 经连接 / 链路发送没成功（对方已经关了；发送侧报 Full 的也算，没发出去）
    std::uint64_t error_replies = 0;   // 回给调用方的错误 Response 数
    std::uint64_t local_nodes = 0;     // 此刻登记的本机 Node 数
};

class AgentRouter final : public ShmChannelHandler, public IConnectionHandler
{
public:
    explicit AgentRouter(RouteTable& table, AgentRouterOptions options = {});

    // ---- ShmChannelHandler（共享内存的通道）----
    int OnAdmit(const ShmAdmitRequest&) noexcept override;
    void OnOpened(ShmServerChannel&) noexcept override;
    bool OnReadable(ShmServerChannel&) noexcept override;
    void OnClosed(ShmServerChannel&, ShmCloseReason) noexcept override;

    // ---- IConnectionHandler（仅 UDS 的 App，以及别的 Agent 的链路）----
    // 连接带着 UdsPeerContext 的是本机仅 UDS 的 App；没有的是别的 Agent 的链路。
    void OnConnected(IConnection&) noexcept override;
    void OnMessage(IConnection&, std::span<const std::byte> payload) noexcept override;
    void OnClosed(IConnection&, CloseReason, int) noexcept override;

    AgentRouterStats stats() const noexcept;

private:
    struct LocalPeer
    {
        NodeId node = 0;
        std::uint64_t channel_id = 0;
        std::string busid;
        ShmServerChannel* shm = nullptr; // 共享内存的对端
        IConnection* conn = nullptr;      // 仅 UDS 的对端
    };

    // 一条消息从哪来。
    struct Source
    {
        enum class Kind : std::uint8_t
        {
            kShm,
            kUds,
            kLink
        };
        Kind kind;
        NodeId node = 0;            // 本机 App 的 NodeId（kLink 无意义）
        ShmServerChannel* shm = nullptr;
        IConnection* conn = nullptr;
    };

    // 登记 / 撤销本机的 Node。
    void Register(NodeId node, std::uint64_t channel_id, std::string_view busid, ShmServerChannel* shm, IConnection* conn);
    void Unregister(NodeId node, std::uint64_t channel_id);
    std::optional<NodeId> NodeOf(std::string_view busid, const PeerCredentials& cred) const;

    // 处理一条消息：bytes 是信封 + 业务载荷；enqueue_ts_ns 是它的入队时刻（不是从环里来的传"收到的时刻"）。
    void Route(const Source& src, std::span<const std::byte> bytes, std::uint64_t enqueue_ts_ns);
    enum class Delivery : std::uint8_t
    {
        kOk,
        kNoRoute,
        kUnavailable
    };
    // head 是已经校验、改写好的头；body 是业务载荷。
    Delivery DeliverTo(NodeId node, const EnvelopeHead& head, std::span<const std::byte> body);
    Delivery DeliverShm(ShmServerChannel& ch, const EnvelopeHead& head, std::span<const std::byte> body);
    Delivery DeliverBytes(IConnection& c, const EnvelopeHead& head, std::span<const std::byte> body);
    void ReplyError(const Source& src, const EnvelopeHead& req, std::uint32_t err);

    std::uint64_t NowNs() const;

    // 把 head + body 拼进一块连续内存（scratch_），返回视图。
    std::span<const std::byte> Assemble(const EnvelopeHead& head, std::span<const std::byte> body);

    RouteTable& table_;
    AgentRouterOptions options_;
    std::unordered_map<NodeId, LocalPeer> nodes_;           // 本机的 Node → 对端
    std::unordered_map<std::uint64_t, NodeId> channel_node_; // 共享内存通道 id → NodeId（来源核对用）
    std::vector<std::byte> scratch_;                        // 只在 loop 线程用
    std::vector<NodeId> targets_;                           // 同上，复用避免每条分配

    struct Counters
    {
        std::atomic<std::uint64_t> received_shm{0}, received_uds{0}, received_link{0};
        std::atomic<std::uint64_t> delivered_shm{0}, delivered_uds{0}, delivered_link{0};
        std::atomic<std::uint64_t> bad_envelope{0}, bad_src{0}, stale_drops{0}, invalid_expire{0}, hops_exceeded{0};
        std::atomic<std::uint64_t> no_route{0}, rx_full{0}, rx_full_detached{0}, too_large{0}, send_closed{0};
        std::atomic<std::uint64_t> error_replies{0}, local_nodes{0};
    } c_;
};

} // namespace breeze::agent
