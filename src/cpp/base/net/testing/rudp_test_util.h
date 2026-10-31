// rudp 测试共用的小工具（只给 *_test.cpp 用，不进库）：原始 UDP 对端、可丢包 / 复制 / 乱序的 UDP 代理。
// 都是阻塞风格 + 带超时，卡死会失败而不是挂住 ctest。

#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "base/net/rudp/internal/kcp_session.h"
#include "base/net/rudp/rudp_endpoint.h"
#include "base/net/rudp/rudp_options.h"
#include "base/net/rudp/rudp_protocol.h"
#include "base/net/rudp/rudp_ticket.h"
#include "base/net/socket/socket_address.h"
#include "base/net/socket/unique_fd.h"
#include "base/net/testing/net_test_util.h"

namespace breeze::testing
{

using Bytes = std::vector<std::byte>;

inline Bytes ToBytes(std::string_view s)
{
    return {reinterpret_cast<const std::byte*>(s.data()), reinterpret_cast<const std::byte*>(s.data()) + s.size()};
}

inline Endpoint Loopback(std::uint16_t port = 0)
{
    const auto ep = Endpoint::Udp("127.0.0.1", port);
    CHECK(ep.has_value());
    return *ep;
}

inline RUdpEndpoint RUdpLoopback(std::uint16_t port = 0)
{
    const auto ep = RUdpEndpoint::Make("127.0.0.1", port);
    CHECK(ep.has_value());
    return *ep;
}

// 原始 UDP 对端：直接收发数据报，用来冒充"不按规矩来的客户端"，或当服务端的对手。
class RawUdp
{
public:
    // port 为 0：系统分配。
    explicit RawUdp(std::uint16_t port = 0)
    {
        CHECK(sock::CreateSocket(Loopback(), SOCK_DGRAM, fd_) == 0);
        sockaddr_storage ss;
        const socklen_t len = sock::ToSockaddr(Loopback(port), ss);
        CHECK(::bind(fd_.get(), reinterpret_cast<const sockaddr*>(&ss), len) == 0);
        sockaddr_storage got;
        socklen_t gl = sizeof(got);
        CHECK(::getsockname(fd_.get(), reinterpret_cast<sockaddr*>(&got), &gl) == 0);
        local_ = *sock::FromSockaddr(got, gl, Endpoint::Kind::Udp);
    }

    const Endpoint& local() const { return local_; }
    int fd() const { return fd_.get(); }

    void SendTo(const Endpoint& to, std::span<const std::byte> data)
    {
        sockaddr_storage ss;
        const socklen_t len = sock::ToSockaddr(to, ss);
        CHECK(::sendto(fd_.get(), data.data(), data.size(), 0, reinterpret_cast<const sockaddr*>(&ss), len) ==
              static_cast<ssize_t>(data.size()));
    }
    void SendTo(const Endpoint& to, std::string_view s) { SendTo(to, ToBytes(s)); }

    // 收一个数据报；超时返回空。from 非空时填来源。
    std::optional<Bytes> Recv(std::chrono::milliseconds timeout = kWait, Endpoint* from = nullptr)
    {
        pollfd p{fd_.get(), POLLIN, 0};
        if (::poll(&p, 1, static_cast<int>(timeout.count())) <= 0)
            return std::nullopt;
        std::byte buf[65536];
        sockaddr_storage ss;
        socklen_t len = sizeof(ss);
        const ssize_t n = ::recvfrom(fd_.get(), buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&ss), &len);
        if (n < 0)
            return std::nullopt;
        if (from != nullptr)
            *from = *sock::FromSockaddr(ss, len, Endpoint::Kind::Udp);
        return Bytes(buf, buf + n);
    }

    // 在 timeout 内一个数据报都没收到才返回 true。
    bool ExpectSilence(std::chrono::milliseconds timeout = 300ms) { return !Recv(timeout).has_value(); }

    // 丢掉当前所有待收的数据报，返回个数。
    std::size_t Drain()
    {
        std::size_t n = 0;
        while (Recv(0ms).has_value())
            ++n;
        return n;
    }

private:
    UniqueFd fd_;
    Endpoint local_ = Loopback();
};

// UDP 代理：客户端连代理的端口，代理转发给真实服务端，两个方向都可以按规则丢包、复制、乱序。
// 一个转发线程，poll 驱动；每个客户端来源对应一个连向服务端的 socket。规则可随时改（原子）。
// 随机数是确定性的（固定种子的线性同余），同样的规则同样的流量得到同样的丢包序列（线程调度不同仍可能小有差别）。
class UdpProxy
{
public:
    struct Rules
    {
        int loss_up_pct = 0;    // 客户端 → 服务端 丢包率（%）
        int loss_down_pct = 0;  // 服务端 → 客户端
        int dup_pct = 0;        // 两个方向的复制率
        int reorder_pct = 0;    // 两个方向：这个包先扣住，等下一个包过去之后再放
        bool blackhole = false; // 两个方向全丢
    };

    explicit UdpProxy(const Endpoint& server)
        : server_(server)
    {
        CHECK(sock::CreateSocket(Loopback(), SOCK_DGRAM, front_) == 0);
        sockaddr_storage ss;
        const socklen_t len = sock::ToSockaddr(Loopback(), ss);
        CHECK(::bind(front_.get(), reinterpret_cast<const sockaddr*>(&ss), len) == 0);
        sockaddr_storage got;
        socklen_t gl = sizeof(got);
        CHECK(::getsockname(front_.get(), reinterpret_cast<sockaddr*>(&got), &gl) == 0);
        endpoint_ = *sock::FromSockaddr(got, gl, Endpoint::Kind::Udp);
        thread_ = std::thread([this] { Run(); });
    }
    ~UdpProxy()
    {
        stop_ = true;
        thread_.join();
    }
    UdpProxy(const UdpProxy&) = delete;
    UdpProxy& operator=(const UdpProxy&) = delete;

    // 客户端应连这个地址。
    const Endpoint& endpoint() const { return endpoint_; }
    void SetRules(const Rules& r)
    {
        loss_up_ = r.loss_up_pct;
        loss_down_ = r.loss_down_pct;
        dup_ = r.dup_pct;
        reorder_ = r.reorder_pct;
        blackhole_ = r.blackhole;
    }
    std::uint64_t forwarded() const { return forwarded_.load(); }
    std::uint64_t dropped() const { return dropped_.load(); }

private:
    struct Client
    {
        sockaddr_storage addr{};
        socklen_t len = 0;
        UniqueFd back; // connect 到服务端
        Bytes held_up, held_down;
        bool has_up = false, has_down = false;
    };

    bool Roll(int pct)
    {
        if (pct <= 0)
            return false;
        rng_ = rng_ * 1664525u + 1013904223u;
        return static_cast<int>((rng_ >> 16) % 100) < pct;
    }

    void Out(int fd, const sockaddr_storage* to, socklen_t to_len, const Bytes& d)
    {
        (void)::sendto(fd, d.data(), d.size(), 0, reinterpret_cast<const sockaddr*>(to), to_len);
        forwarded_.fetch_add(1);
    }

    // 对一个数据报应用规则后发出。held/has_held 是该方向被扣住的那个。
    void Forward(int fd, const sockaddr_storage* to, socklen_t to_len, Bytes d, int loss, Bytes& held, bool& has_held)
    {
        if (blackhole_.load() || Roll(loss))
        {
            dropped_.fetch_add(1);
            return;
        }
        const bool dup = Roll(dup_.load());
        if (!has_held && Roll(reorder_.load()))
        {
            held = std::move(d); // 扣住，等下一个包过去之后再放
            has_held = true;
            return;
        }
        Out(fd, to, to_len, d);
        if (dup)
            Out(fd, to, to_len, d);
        if (has_held)
        {
            Out(fd, to, to_len, held);
            has_held = false;
        }
    }

    void Run()
    {
        std::byte buf[65536];
        while (!stop_.load())
        {
            std::vector<pollfd> fds;
            std::vector<Client*> owners;
            fds.push_back({front_.get(), POLLIN, 0});
            owners.push_back(nullptr);
            for (auto& [k, c] : clients_)
            {
                fds.push_back({c.back.get(), POLLIN, 0});
                owners.push_back(&c);
            }
            if (::poll(fds.data(), fds.size(), 5) <= 0)
                continue;
            for (std::size_t i = 0; i < fds.size(); ++i)
            {
                if (!(fds[i].revents & POLLIN))
                    continue;
                sockaddr_storage ss;
                socklen_t len = sizeof(ss);
                const ssize_t n = ::recvfrom(fds[i].fd, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&ss), &len);
                if (n < 0)
                    continue;
                Bytes d(buf, buf + n);
                if (owners[i] == nullptr)
                {
                    // 客户端 → 服务端：按来源找 / 建对应的上游 socket
                    const std::string key(reinterpret_cast<const char*>(&ss), len);
                    auto it = clients_.find(key);
                    if (it == clients_.end())
                    {
                        Client c;
                        c.addr = ss;
                        c.len = len;
                        CHECK(sock::CreateSocket(Loopback(), SOCK_DGRAM, c.back) == 0);
                        sockaddr_storage srv;
                        const socklen_t sl = sock::ToSockaddr(server_, srv);
                        CHECK(::connect(c.back.get(), reinterpret_cast<const sockaddr*>(&srv), sl) == 0);
                        it = clients_.emplace(key, std::move(c)).first;
                    }
                    Client& c = it->second;
                    Forward(c.back.get(), nullptr, 0, std::move(d), loss_up_.load(), c.held_up, c.has_up);
                }
                else
                {
                    Client& c = *owners[i];
                    Forward(front_.get(), &c.addr, c.len, std::move(d), loss_down_.load(), c.held_down, c.has_down);
                }
            }
        }
    }

    Endpoint server_;
    Endpoint endpoint_ = Loopback();
    UniqueFd front_;
    std::map<std::string, Client> clients_; // 只在转发线程访问
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<int> loss_up_{0}, loss_down_{0}, dup_{0}, reorder_{0};
    std::atomic<bool> blackhole_{false};
    std::atomic<std::uint64_t> forwarded_{0}, dropped_{0};
    std::uint32_t rng_ = 20240607;
};

// 在原始 UDP 对端上跑真实的 KCP 会话，冒充一个客户端（或服务端）：能握手、收发消息、也能故意不守规矩（不回 ack、
// 发超限消息、乱发控制包）。时间由 Run 驱动，每 10ms 推进一次 KCP，期间收到的数据报分流：
// Data 喂进 KCP、完整的消息记入 messages；其它包记入 controls。
class KcpPeer
{
public:
    struct Control
    {
        rudp::PacketType type;
        std::uint32_t conv;
        std::uint8_t reason;
    };

    KcpPeer(RawUdp& udp, const Endpoint& server, std::uint32_t conv, const RUdpOptions& options = {})
        : udp_(udp)
        , server_(server)
        , conv_(conv)
    {
        kcp_ = KcpSession::Create(conv, options, &KcpPeer::Output, this);
        CHECK(kcp_ != nullptr);
        kcp_->Update(KcpNowMs());
    }

    std::uint32_t conv() const { return conv_; }
    KcpSession& kcp() { return *kcp_; }

    // 发 Hello，等 Accept（期间的其它包记入 controls）。
    // 返回是否收到 Accept；收到 Reject 返回 false，原因在 last_reject()。
    bool Handshake(const RUdpTicket& ticket, std::chrono::milliseconds timeout = 2s)
    {
        const auto c = rudp::EncodeHello(ticket.conv, ticket.token);
        udp_.SendTo(server_, c.view());
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            const std::size_t before = controls_.size();
            Run(20ms);
            for (std::size_t i = before; i < controls_.size(); ++i)
            {
                if (controls_[i].type == rudp::PacketType::Accept)
                    return true;
                if (controls_[i].type == rudp::PacketType::Reject)
                {
                    last_reject_ = static_cast<rudp::RejectReason>(controls_[i].reason);
                    return false;
                }
            }
        }
        return false;
    }
    rudp::RejectReason last_reject() const { return last_reject_; }

    void Send(std::string_view msg)
    {
        std::uint32_t frags = 0;
        CHECK(kcp_->Send(ToBytes(msg), frags) == 0);
    }
    void SendControl(const rudp::Control& c) { udp_.SendTo(server_, c.view()); }
    void SendRaw(std::span<const std::byte> d) { udp_.SendTo(server_, d); }

    // 驱动 d 毫秒。
    void Run(std::chrono::milliseconds d)
    {
        const auto until = std::chrono::steady_clock::now() + d;
        do
        {
            kcp_->Update(KcpNowMs());
            const auto remain =
                std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now());
            const auto wait =
                std::min<std::chrono::milliseconds>(std::max<std::chrono::milliseconds>(remain, 0ms), 5ms);
            auto dg = udp_.Recv(wait);
            while (dg.has_value())
            {
                OnDatagram(*dg);
                dg = udp_.Recv(0ms); // 已经到的一次取空
            }
        } while (std::chrono::steady_clock::now() < until);
    }

    template <typename Pred>
    bool RunUntil(Pred pred, std::chrono::milliseconds timeout = kWait)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!pred())
        {
            if (std::chrono::steady_clock::now() > deadline)
                return false;
            Run(10ms);
        }
        return true;
    }

    const std::vector<std::string>& messages() const { return messages_; }
    const std::vector<Control>& controls() const { return controls_; }
    std::size_t CountControls(rudp::PacketType t) const
    {
        std::size_t n = 0;
        for (const auto& c : controls_)
            n += c.type == t ? 1 : 0;
        return n;
    }

private:
    static void Output(void* user, std::span<const std::byte> seg)
    {
        auto* self = static_cast<KcpPeer*>(user);
        Bytes d;
        d.reserve(seg.size() + 1);
        d.push_back(rudp::DataHeader());
        d.insert(d.end(), seg.begin(), seg.end());
        self->udp_.SendTo(self->server_, d);
    }

    void OnDatagram(const Bytes& dg)
    {
        const auto p = rudp::Parse(dg);
        if (!p)
            return;
        if (p->type == rudp::PacketType::Data)
        {
            if (kcp_->Input(p->body, KcpNowMs()) < 0)
                return;
            for (;;)
            {
                const int size = kcp_->PeekSize();
                if (size < 0)
                    break;
                Bytes buf(static_cast<std::size_t>(size));
                CHECK(kcp_->Recv(buf) == size);
                messages_.emplace_back(reinterpret_cast<const char*>(buf.data()), buf.size());
            }
            return;
        }
        controls_.push_back({p->type, p->conv, p->reason});
    }

    RawUdp& udp_;
    Endpoint server_;
    std::uint32_t conv_;
    std::unique_ptr<KcpSession> kcp_;
    std::vector<std::string> messages_;
    std::vector<Control> controls_;
    rudp::RejectReason last_reject_{};
};

} // namespace breeze::testing
