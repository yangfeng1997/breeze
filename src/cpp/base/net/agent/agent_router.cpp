#include "base/net/agent/agent_router.h"

#include <charconv>
#include <cstring>

#include "base/buffer/shared_buffer.h"
#include "base/logger/logger.h"
#include "base/logger/logger_macros.h"
#include "base/net/connection/connection_interface.h"
#include "base/time/now.h"

namespace breeze::agent
{

namespace
{

// 通道名按十进制解析成 NodeId：整串都是数字、不为 0、不溢出 u32。
std::optional<NodeId> ParseDecimalNode(std::string_view s) noexcept
{
    std::uint32_t v = 0;
    const char* end = s.data() + s.size();
    const auto r = std::from_chars(s.data(), end, v);
    if (r.ec != std::errc{} || r.ptr != end || v == 0)
        return std::nullopt;
    return v;
}

void Bump(std::atomic<std::uint64_t>& c) noexcept { c.fetch_add(1, std::memory_order_relaxed); }

} // namespace

AgentRouter::AgentRouter(RouteTable& table, AgentRouterOptions options)
    : table_(table)
    , options_(std::move(options))
{
}

std::uint64_t AgentRouter::NowNs() const
{
    return options_.clock ? options_.clock() : static_cast<std::uint64_t>(Now::SteadyNs());
}

std::optional<NodeId> AgentRouter::NodeOf(std::string_view busid, const PeerCredentials& cred) const
{
    if (options_.node_of)
        return options_.node_of(busid, cred);
    return ParseDecimalNode(busid);
}

// ---- 对端的登记 ----

void AgentRouter::Register(
    NodeId node, std::uint64_t channel_id, std::string_view busid, ShmServerChannel* shm, IConnection* conn)
{
    // 覆盖：discard_retained 重建时新通道先于旧通道的 OnClosed 出现，旧的撤销时按 channel_id 核对，不会把新的删掉。
    nodes_[node] = LocalPeer{node, channel_id, std::string(busid), shm, conn};
    channel_node_[channel_id] = node;
    c_.local_nodes.store(nodes_.size(), std::memory_order_relaxed);
}

void AgentRouter::Unregister(NodeId node, std::uint64_t channel_id)
{
    channel_node_.erase(channel_id);
    if (const auto it = nodes_.find(node); it != nodes_.end() && it->second.channel_id == channel_id)
        nodes_.erase(it);
    c_.local_nodes.store(nodes_.size(), std::memory_order_relaxed);
}

int AgentRouter::OnAdmit(const ShmAdmitRequest& req) noexcept
{
    const auto node = NodeOf(req.busid, req.peer);
    if (!node)
        return EACCES; // 不认识这个通道名：它不是我们的 Node
    // 一个 Node 同一时刻只能有一条通道。同一个通道名的重复 / 恢复 / 丢弃重建由服务端判（EBUSY、恢复、Discarded）；
    // 这里只拦"另一个通道名映射到了同一个 Node"。
    if (const auto it = nodes_.find(*node); it != nodes_.end() && it->second.busid != req.busid)
        return EBUSY;
    return 0;
}

void AgentRouter::OnOpened(ShmServerChannel& ch) noexcept
{
    const auto node = NodeOf(ch.busid(), ch.peer());
    if (!node) // 准入过了就一定认识；防御
    {
        ch.Close();
        return;
    }
    Register(*node, ch.id(), ch.busid(), &ch, nullptr);
}

void AgentRouter::OnClosed(ShmServerChannel& ch, ShmCloseReason) noexcept
{
    if (const auto it = channel_node_.find(ch.id()); it != channel_node_.end())
        Unregister(it->second, ch.id());
}

void AgentRouter::OnConnected(IConnection& conn) noexcept
{
    const UdsPeerContext* ctx = conn.GetContext<UdsPeerContext>();
    if (ctx == nullptr)
        return; // 别的 Agent 的链路：不是本机的 Node，不登记
    const auto node = NodeOf(ctx->busid, ctx->cred);
    if (!node)
    {
        conn.Close();
        return;
    }
    Register(*node, ctx->channel_id, ctx->busid, nullptr, &conn);
}

void AgentRouter::OnClosed(IConnection& conn, CloseReason, int) noexcept
{
    const UdsPeerContext* ctx = conn.GetContext<UdsPeerContext>();
    if (ctx == nullptr)
        return;
    if (const auto it = channel_node_.find(ctx->channel_id); it != channel_node_.end())
        Unregister(it->second, ctx->channel_id);
}

// ---- 收 ----

bool AgentRouter::OnReadable(ShmServerChannel& ch) noexcept
{
    const auto it = channel_node_.find(ch.id());
    const NodeId node = it == channel_node_.end() ? 0 : it->second;
    for (std::size_t left = options_.read_budget; left > 0; --left)
    {
        shm::RecordView v;
        const shm::PeekStatus st = ch.tx().Peek(v);
        if (st == shm::PeekStatus::kEmpty)
            return false;
        if (st == shm::PeekStatus::kCorrupt)
        {
            ch.Close(); // 对端写坏了环：断开，不尝试修复
            return false;
        }
        // 先标记再处理，处理完立刻 Pop：这时 Agent 崩溃，新 Agent 靠标记跳过这一条（至多一次，也不会被毒包拖进崩溃循环）。
        ch.tx().MarkInflight(v);
        Bump(c_.received_shm);
        Route(Source{Source::Kind::kShm, node, &ch, nullptr}, std::span(v.payload, v.len), v.enqueue_ts_ns);
        ch.tx().Pop();
    }
    return true;
}

void AgentRouter::OnMessage(IConnection& conn, std::span<const std::byte> payload) noexcept
{
    const std::uint64_t now = NowNs();
    const UdsPeerContext* ctx = conn.GetContext<UdsPeerContext>();
    if (ctx == nullptr)
    {
        Bump(c_.received_link);
        Route(Source{Source::Kind::kLink, 0, nullptr, &conn}, payload, now);
        return;
    }
    const auto it = channel_node_.find(ctx->channel_id);
    if (it == channel_node_.end())
        return; // 没登记成功（节点不认识）的连接在 OnConnected 里已经关了
    Bump(c_.received_uds);
    Route(Source{Source::Kind::kUds, it->second, nullptr, &conn}, payload, now);
}

// ---- 路由 ----

void AgentRouter::Route(const Source& src, std::span<const std::byte> bytes, std::uint64_t enqueue_ts_ns)
{
    EnvelopeHead head;
    if (const EnvStatus st = ParseEnvelope(bytes, head); st != EnvStatus::kOk)
    {
        Bump(c_.bad_envelope);
        LOG_DEBUG("agent: 丢弃信封不合法的消息 原因={} 来源={}", ToString(st), static_cast<int>(src.kind));
        return;
    }
    // 本机 App 的 src 必须是它自己（握手时登记的 NodeId）；别的 Agent 的链路是受信的。
    if (src.kind != Source::Kind::kLink && head.src != src.node)
    {
        Bump(c_.bad_src);
        return;
    }
    std::uint32_t left_ms = 0;
    switch (CheckExpire(head.remaining_ms, enqueue_ts_ns, NowNs(), &left_ms))
    {
        case ExpireStatus::kInvalid:
            Bump(c_.invalid_expire);
            return;
        case ExpireStatus::kExpired:
            Bump(c_.stale_drops); // 过期不执行、不回包（请求方早已判超时；单向本来就可丢）
            return;
        case ExpireStatus::kAlive:
            break;
    }
    if (!PatchForForward(head, left_ms))
    {
        Bump(c_.hops_exceeded);
        return;
    }
    const std::span<const std::byte> body = bytes.subspan(kEnvelopeHeadSize);
    const bool is_request = head.type == static_cast<std::uint8_t>(EnvType::kRequest);

    targets_.clear();
    table_.Resolve(head, targets_);
    if (targets_.empty())
    {
        Bump(c_.no_route);
        if (is_request)
            ReplyError(src, head, kRouteErrNoRoute);
        return;
    }
    bool any_ok = false;
    Delivery worst = Delivery::kOk;
    for (const NodeId node : targets_)
    {
        const Delivery d = DeliverTo(node, head, body);
        if (d == Delivery::kOk)
            any_ok = true;
        else if (worst == Delivery::kOk || d == Delivery::kUnavailable)
            worst = d;
    }
    // 一个都没送到的 Request：回错误，让调用方快速失败。Oneway / Response 不通知（可丢）。
    if (!any_ok && is_request)
        ReplyError(src, head, worst == Delivery::kNoRoute ? kRouteErrNoRoute : kRouteErrUnavailable);
}

std::span<const std::byte> AgentRouter::Assemble(const EnvelopeHead& head, std::span<const std::byte> body)
{
    scratch_.resize(kEnvelopeHeadSize + body.size());
    WriteEnvelopeHead(scratch_, head);
    if (!body.empty())
        std::memcpy(scratch_.data() + kEnvelopeHeadSize, body.data(), body.size());
    return scratch_;
}

AgentRouter::Delivery AgentRouter::DeliverTo(NodeId node, const EnvelopeHead& head, std::span<const std::byte> body)
{
    if (const auto it = nodes_.find(node); it != nodes_.end())
    {
        LocalPeer& p = it->second;
        return p.shm != nullptr ? DeliverShm(*p.shm, head, body) : DeliverBytes(*p.conn, head, body);
    }
    // 不是本机的：看是不是在别的 Agent 上。
    const ConnectionHandle link = table_.RemoteOf(node);
    if (link.raw() != 0)
    {
        if (link.Send(SharedBuffer::CopyFrom(Assemble(head, body))) == SendResult::Ok)
        {
            Bump(c_.delivered_link);
            return Delivery::kOk;
        }
        Bump(c_.send_closed);
        return Delivery::kUnavailable;
    }
    Bump(c_.no_route);
    return Delivery::kNoRoute;
}

// 直接写进目的地的 Rx 环：改写过的头与载荷各拷一次，不经中转队列。App 在保留中（Detached）时同样写，满了丢。
AgentRouter::Delivery AgentRouter::DeliverShm(ShmServerChannel& ch, const EnvelopeHead& head, std::span<const std::byte> body)
{
    const std::size_t total = kEnvelopeHeadSize + body.size();
    if (total > UINT32_MAX)
    {
        Bump(c_.too_large);
        return Delivery::kUnavailable;
    }
    shm::Reserved r = ch.rx().TryReserve(static_cast<std::uint32_t>(total));
    switch (r.status)
    {
        case shm::ReserveStatus::kOk:
            break;
        case shm::ReserveStatus::kFull:
            Bump(ch.detached() ? c_.rx_full_detached : c_.rx_full);
            return Delivery::kUnavailable;
        case shm::ReserveStatus::kTooLarge:
            Bump(c_.too_large);
            return Delivery::kUnavailable;
        case shm::ReserveStatus::kCorrupt:
            ch.Close(); // Rx 环的 read_idx 被对端写坏了
            return Delivery::kUnavailable;
    }
    WriteEnvelopeHead(r.payload, head);
    if (!body.empty())
        std::memcpy(r.payload.data() + kEnvelopeHeadSize, body.data(), body.size());
    ch.rx().Commit();
    Bump(c_.delivered_shm);
    return Delivery::kOk;
}

AgentRouter::Delivery AgentRouter::DeliverBytes(IConnection& c, const EnvelopeHead& head, std::span<const std::byte> body)
{
    if (c.Send(Assemble(head, body)) == SendResult::Ok)
    {
        Bump(c_.delivered_uds);
        return Delivery::kOk;
    }
    Bump(c_.send_closed);
    return Delivery::kUnavailable;
}

void AgentRouter::ReplyError(const Source& src, const EnvelopeHead& req, std::uint32_t err)
{
    if (req.src == 0)
        return; // 请求没有发送方，没法回
    EnvelopeHead h{};
    h.magic = kEnvelopeMagic;
    h.version = kEnvelopeVersion;
    h.type = static_cast<std::uint8_t>(EnvType::kResponse);
    h.flags = req.flags & kEnvFlagTrace;
    h.body_len = 0;
    h.msg_id = req.msg_id;
    h.src = req.dst; // 请求点名的目的地（Direct 时有意义，否则是 0）
    h.dst = req.src;
    h.route_type = static_cast<std::uint8_t>(RouteType::kDirect);
    h.remaining_ms = options_.error_reply_ttl_ms;
    h.err_code = err;
    h.seq = req.seq;
    h.trace_id = req.trace_id;
    Delivery d = Delivery::kUnavailable;
    switch (src.kind)
    {
        case Source::Kind::kShm:
            d = DeliverShm(*src.shm, h, {});
            break;
        case Source::Kind::kUds:
        case Source::Kind::kLink: // 链路上回去：对面的 Agent 按 dst 路由
            d = DeliverBytes(*src.conn, h, {});
            break;
    }
    if (d == Delivery::kOk)
        Bump(c_.error_replies);
}

AgentRouterStats AgentRouter::stats() const noexcept
{
    const auto ld = [](const std::atomic<std::uint64_t>& a) { return a.load(std::memory_order_relaxed); };
    AgentRouterStats s;
    s.received_shm = ld(c_.received_shm);
    s.received_uds = ld(c_.received_uds);
    s.received_link = ld(c_.received_link);
    s.delivered_shm = ld(c_.delivered_shm);
    s.delivered_uds = ld(c_.delivered_uds);
    s.delivered_link = ld(c_.delivered_link);
    s.bad_envelope = ld(c_.bad_envelope);
    s.bad_src = ld(c_.bad_src);
    s.stale_drops = ld(c_.stale_drops);
    s.invalid_expire = ld(c_.invalid_expire);
    s.hops_exceeded = ld(c_.hops_exceeded);
    s.no_route = ld(c_.no_route);
    s.rx_full = ld(c_.rx_full);
    s.rx_full_detached = ld(c_.rx_full_detached);
    s.too_large = ld(c_.too_large);
    s.send_closed = ld(c_.send_closed);
    s.error_replies = ld(c_.error_replies);
    s.local_nodes = ld(c_.local_nodes);
    return s;
}

} // namespace breeze::agent
