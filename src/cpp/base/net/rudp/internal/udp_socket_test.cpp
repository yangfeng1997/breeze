// UdpSocket：收（批量、来源地址、超大数据报丢弃、公平上限）、发（包头与载荷拼成一个包）、客户端的 connect 过滤、
// ICMP 错误上报、Close 与端口释放、套接字缓冲、字节统计。
// loop 由测试线程自己 RunOnce 驱动，单线程、结果可复现。无测试框架依赖：失败即 abort。

#include "base/net/rudp/internal/udp_socket.h"

#include <sys/socket.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <mutex>

#include "base/net/loop/event_loop.h"
#include "base/net/socket/socket_address.h"
#include "base/net/testing/rudp_test_util.h"

using namespace breeze;
using namespace breeze::testing;

namespace
{

class Recorder final : public IDatagramHandler
{
public:
    struct Got
    {
        Endpoint from;
        Bytes data;
    };

    void OnDatagram(const sockaddr_storage& src, socklen_t len, std::span<const std::byte> d) override
    {
        const std::lock_guard lk(mu_);
        got_.push_back({*sock::FromSockaddr(src, len, Endpoint::Kind::Udp), Bytes(d.begin(), d.end())});
    }
    void OnSocketError(int err) override
    {
        const std::lock_guard lk(mu_);
        errors_.push_back(err);
    }

    std::size_t size() const
    {
        const std::lock_guard lk(mu_);
        return got_.size();
    }
    std::size_t errors() const
    {
        const std::lock_guard lk(mu_);
        return errors_.size();
    }
    Got at(std::size_t i) const
    {
        const std::lock_guard lk(mu_);
        return got_.at(i);
    }
    int error_at(std::size_t i) const
    {
        const std::lock_guard lk(mu_);
        return errors_.at(i);
    }

private:
    mutable std::mutex mu_;
    std::vector<Got> got_;
    std::vector<int> errors_;
};

struct Loop
{
    EventLoop loop;
    Loop() { CHECK(loop.Init() == 0); }

    template <typename Pred>
    bool PumpUntil(Pred pred, std::chrono::milliseconds timeout = 5s)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!pred())
        {
            if (std::chrono::steady_clock::now() > deadline)
                return false;
            loop.RunOnce(10);
        }
        return true;
    }
    void Pump(std::chrono::milliseconds d)
    {
        const auto until = std::chrono::steady_clock::now() + d;
        while (std::chrono::steady_clock::now() < until)
            loop.RunOnce(10);
    }
};

std::unique_ptr<UdpSocket> OpenServer(Loop& l,
                                      Recorder& rec,
                                      const RUdpOptions& o = {},
                                      const Endpoint& ep = Loopback())
{
    std::unique_ptr<UdpSocket> s;
    CHECK(UdpSocket::OpenServer(l.loop, ep, o, rec, s) == 0);
    CHECK(s && s->open());
    return s;
}

std::unique_ptr<UdpSocket> OpenClient(Loop& l, Recorder& rec, const Endpoint& peer, const RUdpOptions& o = {})
{
    std::unique_ptr<UdpSocket> s;
    CHECK(UdpSocket::OpenClient(l.loop, peer, o, rec, s) == 0);
    return s;
}

sockaddr_storage Addr(const Endpoint& ep, socklen_t* len = nullptr)
{
    sockaddr_storage ss;
    const socklen_t n = sock::ToSockaddr(ep, ss);
    if (len != nullptr)
        *len = n;
    return ss;
}

// 端口填 0：系统分配，local() 给出实际端口。
void TestOpenServerAssignsPort()
{
    Loop l;
    Recorder rec;
    auto s = OpenServer(l, rec);
    CHECK(s->local().kind() == Endpoint::Kind::Udp);
    CHECK(s->local().port() != 0);
    CHECK(s->local().address() == "127.0.0.1");
    CHECK(s->fd() >= 0);
}

void TestReceiveSingle()
{
    Loop l;
    Recorder rec;
    auto s = OpenServer(l, rec);
    RawUdp peer;
    peer.SendTo(s->local(), "hello");
    CHECK(l.PumpUntil([&] { return rec.size() == 1; }));
    const auto g = rec.at(0);
    CHECK(g.data == ToBytes("hello"));
    CHECK(g.from == peer.local()); // 来源地址是对端的 ip:port
}

// 一次发一批：全部收到、顺序不变（回环上不乱序），一个也不重复。
void TestReceiveBatch()
{
    Loop l;
    Recorder rec;
    auto s = OpenServer(l, rec);
    RawUdp peer;
    for (int i = 0; i < 50; ++i)
        peer.SendTo(s->local(), "m" + std::to_string(i));
    CHECK(l.PumpUntil([&] { return rec.size() >= 50; }));
    l.Pump(50ms);
    CHECK(rec.size() == 50);
    for (int i = 0; i < 50; ++i)
        CHECK(rec.at(static_cast<std::size_t>(i)).data == ToBytes("m" + std::to_string(i)));
}

// 多个来源：各自的来源地址分得开。
void TestMultipleSources()
{
    Loop l;
    Recorder rec;
    auto s = OpenServer(l, rec);
    RawUdp a, b;
    a.SendTo(s->local(), "from-a");
    b.SendTo(s->local(), "from-b");
    CHECK(l.PumpUntil([&] { return rec.size() == 2; }));
    bool saw_a = false, saw_b = false;
    for (std::size_t i = 0; i < 2; ++i)
    {
        const auto g = rec.at(i);
        if (g.data == ToBytes("from-a"))
            saw_a = g.from == a.local();
        if (g.data == ToBytes("from-b"))
            saw_b = g.from == b.local();
    }
    CHECK(saw_a && saw_b);
}

// 公平上限：一个事件最多读 4 批 × 16 = 64 个数据报，读不完的留给下一轮，免得一个繁忙的 socket 饿死同 loop 上的其它事。
void TestPerEventCap()
{
    Loop l;
    Recorder rec;
    auto s = OpenServer(l, rec);
    RawUdp peer;
    for (int i = 0; i < 200; ++i)
        peer.SendTo(s->local(), "x");
    l.loop.RunOnce(0);
    CHECK(rec.size() == 64);
    CHECK(l.PumpUntil([&] { return rec.size() == 200; }));
}

// 数据报大小：mtu 上限（1500）整个收；超过 1500 的丢掉（包括被内核截断的），丢了之后后续的包照常收。
void TestOversizeDropped()
{
    Loop l;
    Recorder rec;
    auto s = OpenServer(l, rec);
    RawUdp peer;
    peer.SendTo(s->local(), Bytes(1500, std::byte{1}));
    peer.SendTo(s->local(), Bytes(1501, std::byte{2}));
    peer.SendTo(s->local(), Bytes(3000, std::byte{3}));
    peer.SendTo(s->local(), "after");
    CHECK(l.PumpUntil([&] { return rec.size() >= 2; }));
    l.Pump(50ms);
    CHECK(rec.size() == 2);
    CHECK(rec.at(0).data.size() == 1500 && rec.at(0).data[0] == std::byte{1});
    CHECK(rec.at(1).data == ToBytes("after"));
}

// 空数据报也会交给回调（长度 0），由上层的 Parse 丢弃。
void TestEmptyDatagram()
{
    Loop l;
    Recorder rec;
    auto s = OpenServer(l, rec);
    RawUdp peer;
    peer.SendTo(s->local(), std::span<const std::byte>{});
    CHECK(l.PumpUntil([&] { return rec.size() == 1; }));
    CHECK(rec.at(0).data.empty());
}

// 发：包头与载荷拼成一个数据报；任一为空也行；都为空发空数据报。
void TestSendGather()
{
    Loop l;
    Recorder rec;
    auto s = OpenServer(l, rec);
    RawUdp peer;
    socklen_t len = 0;
    const sockaddr_storage to = Addr(peer.local(), &len);
    const Bytes ab = ToBytes("AB"), cde = ToBytes("CDE");

    CHECK(s->Send(ab, cde, &to, len) == 0);
    CHECK(peer.Recv(2s) == ToBytes("ABCDE"));
    CHECK(s->Send({}, cde, &to, len) == 0);
    CHECK(peer.Recv(2s) == ToBytes("CDE"));
    CHECK(s->Send(ab, {}, &to, len) == 0);
    CHECK(peer.Recv(2s) == ToBytes("AB"));
    CHECK(s->Send({}, {}, &to, len) == 0);
    const auto empty = peer.Recv(2s);
    CHECK(empty.has_value() && empty->empty());
}

// 客户端：connect 到服务端，Send 不带地址；服务端看到的来源就是客户端的 local()。
void TestClientSendsToConnectedPeer()
{
    Loop l;
    Recorder srv_rec, cli_rec;
    auto srv = OpenServer(l, srv_rec);
    auto cli = OpenClient(l, cli_rec, srv->local());
    CHECK(cli->local().port() != 0 && cli->local().port() != srv->local().port());
    CHECK(cli->Send({}, ToBytes("ping"), nullptr, 0) == 0);
    CHECK(l.PumpUntil([&] { return srv_rec.size() == 1; }));
    CHECK(srv_rec.at(0).data == ToBytes("ping"));
    CHECK(srv_rec.at(0).from == cli->local());

    // 服务端回包给这个来源，客户端收到
    socklen_t len = 0;
    const sockaddr_storage to = Addr(srv_rec.at(0).from, &len);
    CHECK(srv->Send({}, ToBytes("pong"), &to, len) == 0);
    CHECK(l.PumpUntil([&] { return cli_rec.size() == 1; }));
    CHECK(cli_rec.at(0).data == ToBytes("pong"));
}

// connect 过的 socket，内核只放行来自对端的包：别的来源发来的不会到回调。
void TestConnectedSocketFiltersOtherSources()
{
    Loop l;
    Recorder srv_rec, cli_rec;
    auto srv = OpenServer(l, srv_rec);
    auto cli = OpenClient(l, cli_rec, srv->local());
    RawUdp stranger;
    stranger.SendTo(cli->local(), "spoof");
    l.Pump(100ms);
    CHECK(cli_rec.size() == 0);
}

// 对端端口没人监听：ICMP 不可达经 OnSocketError 报告（ECONNREFUSED），socket 仍然可用，不会因为没读 SO_ERROR 而空转。
void TestIcmpErrorReported()
{
    Loop l;
    Recorder rec;
    Endpoint dead = Loopback();
    {
        RawUdp temp; // 占一个端口再关掉：这个端口没人监听
        dead = temp.local();
    }
    auto cli = OpenClient(l, rec, dead);
    CHECK(cli->Send({}, ToBytes("x"), nullptr, 0) == 0);
    CHECK(l.PumpUntil([&] { return rec.errors() >= 1; }));
    CHECK(rec.error_at(0) == ECONNREFUSED);
    CHECK(cli->open());
    const auto before = l.loop.Iterations();
    l.Pump(100ms);
    // 错误已清除：不再空转（10ms 一轮，100ms 内远少于 1000 轮）
    CHECK(l.loop.Iterations() - before < 200);
    CHECK(cli->Send({}, ToBytes("y"), nullptr, 0) == 0);
}

// Close：摘出 epoll 并关 fd；幂等；之后发送得到 EBADF；端口随之释放，可以重新绑定。
void TestClose()
{
    Loop l;
    Recorder rec;
    auto s = OpenServer(l, rec);
    const Endpoint ep = s->local();
    s->Close();
    CHECK(!s->open());
    s->Close();
    CHECK(s->Send({}, ToBytes("x"), nullptr, 0) == EBADF);

    RawUdp peer;
    peer.SendTo(ep, "late");
    l.Pump(50ms);
    CHECK(rec.size() == 0); // 关了就不再收

    std::unique_ptr<UdpSocket> again;
    CHECK(UdpSocket::OpenServer(l.loop, ep, RUdpOptions{}, rec, again) == 0); // 端口已释放
}

void TestOpenErrors()
{
    Loop l;
    Recorder rec;
    auto s = OpenServer(l, rec);
    std::unique_ptr<UdpSocket> second;
    CHECK(UdpSocket::OpenServer(l.loop, s->local(), RUdpOptions{}, rec, second) == EADDRINUSE);
    CHECK(second == nullptr);
    const auto tcp = Endpoint::Tcp("127.0.0.1", 0);
    CHECK(UdpSocket::OpenServer(l.loop, *tcp, RUdpOptions{}, rec, second) == EPROTONOSUPPORT);
    CHECK(UdpSocket::OpenClient(l.loop, *tcp, RUdpOptions{}, rec, second) == EPROTONOSUPPORT);
    CHECK(second == nullptr);
}

int RecvBuf(int fd)
{
    int v = 0;
    socklen_t n = sizeof(v);
    CHECK(::getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &v, &n) == 0);
    return v;
}

// 套接字缓冲按选项请求：小请求得到小缓冲，0 = 系统默认（比小请求大）。
void TestSocketBuffers()
{
    Loop l;
    Recorder rec;
    RUdpOptions small;
    small.socket_recv_buffer = 4096;
    auto a = OpenServer(l, rec, small);
    RUdpOptions dflt;
    dflt.socket_recv_buffer = 0;
    auto b = OpenServer(l, rec, dflt);
    CHECK(RecvBuf(a->fd()) < 20000);
    CHECK(RecvBuf(b->fd()) > RecvBuf(a->fd()));
}

// 字节统计：收到的与发出的都计入 loop 的 bytes_in / bytes_out。
void TestByteStats()
{
    Loop l;
    Recorder rec;
    auto s = OpenServer(l, rec);
    RawUdp peer;
    peer.SendTo(s->local(), Bytes(100, std::byte{1}));
    peer.SendTo(s->local(), Bytes(23, std::byte{2}));
    CHECK(l.PumpUntil([&] { return rec.size() == 2; }));
    CHECK(l.loop.stats().bytes_in.load() == 123);

    socklen_t len = 0;
    const sockaddr_storage to = Addr(peer.local(), &len);
    CHECK(s->Send(ToBytes("ab"), ToBytes("cdef"), &to, len) == 0);
    CHECK(l.loop.stats().bytes_out.load() == 6);
}

// IPv6 回环（机器没有 IPv6 就跳过）。
void TestIPv6()
{
    Loop l;
    Recorder rec;
    const auto ep = Endpoint::Udp("::1", 0);
    std::unique_ptr<UdpSocket> s;
    const int rc = UdpSocket::OpenServer(l.loop, *ep, RUdpOptions{}, rec, s);
    if (rc == EADDRNOTAVAIL || rc == EAFNOSUPPORT)
    {
        std::puts("udp_socket_test: 本机没有 IPv6，跳过 TestIPv6");
        return;
    }
    CHECK(rc == 0);
    CHECK(s->local().IsIPv6() && s->local().port() != 0);
    Recorder cli_rec;
    auto cli = OpenClient(l, cli_rec, s->local());
    CHECK(cli->Send({}, ToBytes("v6"), nullptr, 0) == 0);
    CHECK(l.PumpUntil([&] { return rec.size() == 1; }));
    CHECK(rec.at(0).data == ToBytes("v6") && rec.at(0).from.IsIPv6());
}

} // namespace

int main()
{
    TestOpenServerAssignsPort();
    TestReceiveSingle();
    TestReceiveBatch();
    TestMultipleSources();
    TestPerEventCap();
    TestOversizeDropped();
    TestEmptyDatagram();
    TestSendGather();
    TestClientSendsToConnectedPeer();
    TestConnectedSocketFiltersOtherSources();
    TestIcmpErrorReported();
    TestClose();
    TestOpenErrors();
    TestSocketBuffers();
    TestByteStats();
    TestIPv6();
    std::puts("udp_socket_test: ok");
    return 0;
}
