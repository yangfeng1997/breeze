#include "kcp_session.h"

#include <ikcp.h>

#include <chrono>
#include <cstddef>

namespace breeze
{

// rudp::kKcpHeader（24）与 kMaxFragments（127）对应 ikcp.c 里的 IKCP_OVERHEAD 与 IKCP_WND_RCV - 1；
// 这两个常量不在 ikcp.h 里，编译期对不上。升级 KCP 若改了它们，kcp_session_test 的 TestMss / TestMaxMessage 会失败。

std::uint32_t KcpNowMs() noexcept
{
    using namespace std::chrono;
    return static_cast<std::uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

int KcpSession::OutputThunk(const char* buf, int len, IKCPCB*, void* user)
{
    auto* self = static_cast<KcpSession*>(user);
    self->out_(self->user_, {reinterpret_cast<const std::byte*>(buf), static_cast<std::size_t>(len)});
    return 0;
}

std::unique_ptr<KcpSession> KcpSession::Create(std::uint32_t conv, const RudpOptions& o, Output out, void* user)
{
    std::unique_ptr<KcpSession> s(new KcpSession);
    s->out_ = out;
    s->user_ = user;
    s->kcp_ = ikcp_create(conv, s.get());
    if (s->kcp_ == nullptr)
        return nullptr; // 析构时 kcp_ 为空，ikcp_release 不会被调
    ikcp_setoutput(s->kcp_, &KcpSession::OutputThunk);
    // mtu 含本库 1 字节包头：KCP 看到的上限要扣掉它，整个数据报才不超过 options.mtu。
    if (ikcp_setmtu(s->kcp_, static_cast<int>(o.mtu - rudp::kDataPrefix)) != 0)
        return nullptr;
    ikcp_wndsize(s->kcp_, static_cast<int>(o.send_window), static_cast<int>(o.recv_window));
    ikcp_nodelay(
        s->kcp_, o.nodelay ? 1 : 0, static_cast<int>(o.interval_ms), o.fast_resend, o.no_congestion_control ? 1 : 0);
    s->kcp_->dead_link = o.dead_link;
    return s;
}

KcpSession::~KcpSession()
{
    if (kcp_ != nullptr)
        ikcp_release(kcp_);
}

int KcpSession::Input(std::span<const std::byte> segment, std::uint32_t now) noexcept
{
    kcp_->current = now; // ikcp 的公开字段：RTT 按它算，Update 之间不拨就是旧的
    return ikcp_input(kcp_, reinterpret_cast<const char*>(segment.data()), static_cast<long>(segment.size()));
}

int KcpSession::Send(std::span<const std::byte> message, std::uint32_t& fragments) noexcept
{
    const std::uint32_t mss = kcp_->mss;
    const auto n = static_cast<std::uint32_t>((message.size() + mss - 1) / mss);
    fragments = n == 0 ? 1 : n; // 空消息也占一段
    return ikcp_send(kcp_, reinterpret_cast<const char*>(message.data()), static_cast<int>(message.size())) < 0 ? -1
                                                                                                                : 0;
}

int KcpSession::PeekSize() const noexcept { return ikcp_peeksize(kcp_); }

int KcpSession::Recv(std::span<std::byte> out) noexcept
{
    return ikcp_recv(kcp_, reinterpret_cast<char*>(out.data()), static_cast<int>(out.size()));
}

void KcpSession::Update(std::uint32_t now) noexcept { ikcp_update(kcp_, now); }

void KcpSession::Flush(std::uint32_t now) noexcept
{
    kcp_->current = now; // ikcp 的公开字段，见头文件注释
    ikcp_flush(kcp_);
}

std::uint32_t KcpSession::WaitSnd() const noexcept { return static_cast<std::uint32_t>(ikcp_waitsnd(kcp_)); }

std::uint32_t KcpSession::QueuedSegments() const noexcept { return kcp_->nsnd_que; }

UpdatePlan KcpSession::PlanUpdate(std::uint32_t now) const noexcept
{
    using Kind = UpdatePlan::Kind;
    // 马上就有事：待发的确认 / 窗口通告 / 排队的数据，下一次 flush 就会发出去
    if (kcp_->ackcount != 0 || kcp_->probe != 0 || kcp_->nsnd_que != 0)
        return {Kind::Soon, now};
    bool any = false;
    std::int32_t best = 0; // 距 now 最近的将来时刻（毫秒）
    // 在途的段：最早的重传时刻。snd_buf 是 IKCPSEG 的链表，node 是它的第一个成员（头文件里公开）。
    static_assert(offsetof(IKCPSEG, node) == 0);
    for (const IQUEUEHEAD* p = kcp_->snd_buf.next; p != &kcp_->snd_buf; p = p->next)
    {
        const auto* seg = reinterpret_cast<const IKCPSEG*>(p);
        const auto d = static_cast<std::int32_t>(seg->resendts - now);
        if (d <= 0)
            return {Kind::Soon, now}; // 已经到点
        if (!any || d < best)
        {
            best = d;
            any = true;
        }
    }
    // 对端窗口为 0：flush 里按 ts_probe 定时探测；探测计时还没起（probe_wait == 0）要先 flush 一次把它起起来
    if (kcp_->rmt_wnd == 0)
    {
        if (kcp_->probe_wait == 0)
            return {Kind::Soon, now};
        const auto d = static_cast<std::int32_t>(kcp_->ts_probe - now);
        if (d <= 0)
            return {Kind::Soon, now};
        if (!any || d < best)
        {
            best = d;
            any = true;
        }
    }
    if (!any)
        return {Kind::Quiescent, 0};
    return {Kind::At, now + static_cast<std::uint32_t>(best)};
}

bool KcpSession::NeedsUpdate() const noexcept
{
    return kcp_->nsnd_buf != 0 || kcp_->nsnd_que != 0 || kcp_->ackcount != 0 || kcp_->probe != 0 || kcp_->rmt_wnd == 0;
}

bool KcpSession::Dead() const noexcept { return kcp_->state != 0; }

std::uint32_t KcpSession::mss() const noexcept { return kcp_->mss; }

} // namespace breeze
