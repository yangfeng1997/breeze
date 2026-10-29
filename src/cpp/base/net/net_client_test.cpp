// NetClient / Connector：退避序列、收发与 fail-fast、断线立即 OnClosed 并重连、连接失败可观测、UDS sidecar 重启、
// connect_timeout（黑洞）、不可重试错误（回到 Idle，回调里 Start() 合法）、Stop / Start 语义、停机与析构。
// 无测试框架依赖：失败即 abort。

#include "net_client.h"

#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "backoff.h"
#include "event_loop_group.h"
#include "length_prefix_codec.h"
#include "net_server.h"
#include "net_test_util.h"

using namespace breeze;
using namespace breeze::testing;

namespace
{

constexpr std::size_t kMaxFrame = 1 << 20;

// 每次连接失败的 (errno, will_retry)。
class FailureLog
{
public:
    ConnectFailedFn Fn()
    {
        return [this](int e, bool retry) noexcept
        {
            const std::lock_guard lk(mu_);
            v_.emplace_back(e, retry);
        };
    }
    std::size_t size() const
    {
        const std::lock_guard lk(mu_);
        return v_.size();
    }
    std::vector<std::pair<int, bool>> all() const
    {
        const std::lock_guard lk(mu_);
        return v_;
    }

private:
    mutable std::mutex mu_;
    std::vector<std::pair<int, bool>> v_;
};

// 服务端：group + 回显的 NetServer。可以在固定端点上反复建起、销毁，模拟对端重启。
struct EchoServer
{
    explicit EchoServer(const Endpoint& ep, bool echo = true, std::uint32_t loops = 1)
        : factory(kMaxFrame)
        , group({loops, 64})
        , server(group, ep, {}, rec, factory)
    {
        if (echo)
            rec.on_message = Echo;
        CHECK(group.Start() == 0);
        CHECK(server.Start() == 0);
    }
    EventRecorder rec;
    LengthPrefixCodecFactory factory;
    EventLoopGroup group;
    NetServer server;
};

// 客户端一侧的 group 与 handler。
struct ClientSide
{
    explicit ClientSide(std::uint32_t loops = 1)
        : factory(kMaxFrame)
        , group({loops, 64})
    {
        CHECK(group.Start() == 0);
    }
    EventRecorder rec;
    LengthPrefixCodecFactory factory;
    EventLoopGroup group;
};

ReconnectPolicy FastPolicy() { return {20ms, 100ms, 2.0, 0.0}; }

// 一个当下没人监听的 TCP 端口（先绑后关）。
Endpoint FreeTcpEndpoint()
{
    Fd fd;
    const Endpoint any = *Endpoint::Tcp("127.0.0.1", 0);
    CHECK(sock::Create(any, fd) == 0);
    CHECK(sock::Bind(fd.get(), any) == 0);
    std::optional<Endpoint> local;
    CHECK(sock::LocalEndpoint(fd.get(), local) == 0 && local);
    return *local;
}

// ---- 退避序列（纯逻辑） ----

void TestBackoffSequence()
{
    // 无抖动：500ms 起翻倍，30s 封顶，Reset 之后从头
    {
        Backoff b(ReconnectPolicy{500ms, 30000ms, 2.0, 0.0}, 1);
        const long long expect[] = {500, 1000, 2000, 4000, 8000, 16000, 30000, 30000, 30000};
        for (const long long e : expect)
            CHECK(b.Next().count() == e);
        b.Reset();
        CHECK(b.Next().count() == 500);
        CHECK(b.Next().count() == 1000);
    }
    // 非整数倍率
    {
        Backoff b(ReconnectPolicy{100ms, 1000ms, 1.5, 0.0}, 1);
        CHECK(b.Next().count() == 100);
        CHECK(b.Next().count() == 150);
        CHECK(b.Next().count() == 225);
        for (int i = 0; i < 20; ++i)
            CHECK(b.Next().count() <= 1000);
        CHECK(b.Next().count() == 1000);
    }
    // 抖动：落在 ±jitter 内、不是常数、均值接近基准；封顶之后也不超过 max_delay
    {
        Backoff b(ReconnectPolicy{1000ms, 1000ms, 1.0, 0.2}, 12345);
        long long lo = 1 << 30, hi = 0;
        for (int i = 0; i < 2000; ++i)
        {
            const long long v = b.Next().count();
            lo = std::min(lo, v);
            hi = std::max(hi, v);
            CHECK(v >= 800 && v <= 1000); // 基准已经是上限，向上的抖动被封顶
        }
        CHECK(hi - lo > 50);
    }
    {
        Backoff b(ReconnectPolicy{1000ms, 60000ms, 1.0, 0.2}, 777);
        long long sum = 0;
        for (int i = 0; i < 2000; ++i)
        {
            const long long v = b.Next().count();
            CHECK(v >= 800 && v <= 1200);
            sum += v;
        }
        CHECK(std::abs(sum / 2000.0 - 1000.0) < 30);
    }
    // 最小 1ms：不会得到 0
    {
        Backoff b(ReconnectPolicy{1ms, 1ms, 1.0, 1.0}, 99);
        for (int i = 0; i < 100; ++i)
            CHECK(b.Next().count() >= 1);
    }
}

void TestPolicyValidity()
{
    CHECK(ReconnectPolicy{}.Valid());
    CHECK(!(ReconnectPolicy{0ms, 100ms, 2.0, 0.1}.Valid()));   // initial_delay 必须 > 0
    CHECK(!(ReconnectPolicy{200ms, 100ms, 2.0, 0.1}.Valid())); // max < initial
    CHECK(!(ReconnectPolicy{10ms, 100ms, 0.5, 0.1}.Valid()));  // multiplier < 1
    CHECK(!(ReconnectPolicy{10ms, 100ms, 2.0, -0.1}.Valid()));
    CHECK(!(ReconnectPolicy{10ms, 100ms, 2.0, 1.5}.Valid()));
}

void TestInvalidOptionsReturnEinval()
{
    const LeakCheck leak;
    ClientSide cs;
    const Endpoint ep = FreeTcpEndpoint();
    {
        ClientOptions co;
        co.connect_timeout = 0ms;
        NetClient c(cs.group, ep, std::move(co), FastPolicy(), cs.rec, cs.factory);
        CHECK(c.Start() == EINVAL);
    }
    {
        NetClient c(cs.group, ep, {}, ReconnectPolicy{0ms, 100ms, 2.0, 0.0}, cs.rec, cs.factory);
        CHECK(c.Start() == EINVAL);
    }
}

// ---- 收发与 fail-fast ----

void TestSendBeforeConnectedIsClosed()
{
    const LeakCheck leak;
    ClientSide cs;
    FailureLog fl;
    ClientOptions co;
    co.on_connect_failed = fl.Fn();
    NetClient c(cs.group, FreeTcpEndpoint(), std::move(co), FastPolicy(), cs.rec, cs.factory);
    CHECK(c.Send(Buf("x")) == SendResult::Closed); // 从未 Start
    CHECK(c.handle().raw() == 0 && !c.handle().valid());
    CHECK(c.Start() == 0);
    CHECK(WaitUntil([&] { return fl.size() >= 1; }));
    CHECK(c.Send(Buf("x")) == SendResult::Closed); // 连不上期间：不暗中缓冲
    CHECK(c.handle().raw() == 0);
    c.Stop();
}

void TestConnectSendReceive(const Endpoint& ep)
{
    const LeakCheck leak;
    EchoServer srv(ep);
    ClientSide cs;
    NetClient c(cs.group, srv.server.endpoint(), {}, FastPolicy(), cs.rec, cs.factory);
    CHECK(c.Start() == 0);
    const ConnectionHandle h = cs.rec.WaitConnected(0);
    CHECK(h.valid() && c.handle() == h);

    CHECK(c.Send(Buf("hello")) == SendResult::Ok);
    CHECK(cs.rec.WaitMessages(1));
    CHECK(cs.rec.payloads()[0] == "hello");
    CHECK(cs.group.GetStats().connected == 1);

    // 可以直接用句柄，也可以用 NetClient::Send；两者等价
    CHECK(h.Send(Buf("again")) == SendResult::Ok);
    CHECK(cs.rec.WaitMessages(2));
    c.Stop();
    CHECK(cs.rec.WaitClosed(0).reason == CloseReason::LocalClose);
    cs.rec.CheckInvariants();
}

// ---- 断线 fail-fast、重连 ----

void TestReconnectAfterPeerCloses()
{
    const LeakCheck leak;
    EchoServer srv(*Endpoint::Tcp("127.0.0.1", 0), /*echo=*/false);
    srv.rec.on_message = [](Connection& c, std::string_view) { c.Close(); };
    std::atomic<int> send_after_closed_in_callback{-1}; // 被钩子引用：要比 cs 活得久（IO 线程到 cs 析构才停）
    ClientSide cs;
    NetClient c(cs.group, srv.server.endpoint(), {}, FastPolicy(), cs.rec, cs.factory);
    cs.rec.on_closed = [&](Connection&, CloseReason)
    {
        // 断开立即 OnClosed：回调里句柄已清，Send 立即 Closed
        send_after_closed_in_callback = static_cast<int>(c.Send(Buf("late")));
    };
    CHECK(c.Start() == 0);
    const ConnectionHandle first = cs.rec.WaitConnected(0);
    CHECK(c.Send(Buf("bye")) == SendResult::Ok);

    const auto closed = cs.rec.WaitClosed(0);
    CHECK(closed.handle == first && closed.reason == CloseReason::PeerClosed);
    CHECK(send_after_closed_in_callback.load() == static_cast<int>(SendResult::Closed));

    const ConnectionHandle second = cs.rec.WaitConnected(1); // 退避之后重连
    CHECK(second != first);                                  // 重连得到新句柄
    CHECK(first.Send(Buf("stale")) == SendResult::Closed);   // 旧句柄不会串到新连接
    CHECK(WaitUntil([&] { return c.handle() == second; }));
    c.Stop();
    cs.rec.CheckInvariants();
}

// 连不上：每次失败都经 on_connect_failed 通知（will_retry = true）；对端起来之后连上，退避重置。
void TestConnectFailureObservableThenConnects()
{
    const LeakCheck leak;
    ClientSide cs;
    FailureLog fl;
    ClientOptions co;
    co.on_connect_failed = fl.Fn();
    const Endpoint ep = FreeTcpEndpoint();
    NetClient c(cs.group, ep, std::move(co), FastPolicy(), cs.rec, cs.factory);
    CHECK(c.Start() == 0);
    CHECK(WaitUntil([&] { return fl.size() >= 3; }));
    for (const auto& [err, retry] : fl.all())
    {
        CHECK(err == ECONNREFUSED);
        CHECK(retry);
    }

    EchoServer srv(ep); // 对端起来了
    CHECK(cs.rec.WaitConnected(0).valid());
    CHECK(c.Send(Buf("hi")) == SendResult::Ok);
    CHECK(cs.rec.WaitMessages(1));
    const std::size_t failures = fl.size();
    std::this_thread::sleep_for(150ms);
    CHECK(fl.size() == failures); // 连上之后不再有失败回调
    c.Stop();
}

// sidecar 重启：UDS 路径在对端没起来时是 ENOENT，按退避重试，起来之后重连成功。
void TestUdsSidecarRestart()
{
    const LeakCheck leak;
    TempDir dir;
    const Endpoint ep = dir.Unix("sidecar.sock");
    ClientSide cs;
    FailureLog fl;
    ClientOptions co;
    co.on_connect_failed = fl.Fn();
    NetClient c(cs.group, ep, std::move(co), FastPolicy(), cs.rec, cs.factory);

    auto srv = std::make_unique<EchoServer>(ep);
    CHECK(c.Start() == 0);
    const ConnectionHandle h1 = cs.rec.WaitConnected(0);
    CHECK(c.Send(Buf("one")) == SendResult::Ok);
    CHECK(cs.rec.WaitMessages(1));

    // sidecar 崩溃：优雅停机（socket 文件随之清掉）→ app 立刻知道
    srv->group.Shutdown(500ms);
    srv.reset();
    const auto closed = cs.rec.WaitClosed(0);
    CHECK(closed.handle == h1);
    CHECK(WaitUntil([&] { return c.handle().raw() == 0; }));
    CHECK(WaitUntil([&] { return fl.size() >= 2; })); // 文件不在：ENOENT 之类，一直重试
    for (const auto& [err, retry] : fl.all())
        CHECK(retry && err != 0);

    srv = std::make_unique<EchoServer>(ep); // sidecar 重启
    const ConnectionHandle h2 = cs.rec.WaitConnected(1);
    CHECK(h2 != h1);
    CHECK(c.Send(Buf("two")) == SendResult::Ok);
    CHECK(cs.rec.WaitMessages(2));
    CHECK(cs.rec.payloads()[1] == "two");
    c.Stop();
    cs.rec.CheckInvariants();
}

// ---- connect_timeout：黑洞 ----

// accept 队列已满的监听 socket：内核丢弃之后的 SYN，客户端 connect 一直挂在 SYN_SENT。
class BlackHole
{
public:
    BlackHole()
    {
        const Endpoint any = *Endpoint::Tcp("127.0.0.1", 0);
        CHECK(sock::Create(any, listen_) == 0);
        CHECK(sock::Bind(listen_.get(), any) == 0);
        CHECK(::listen(listen_.get(), 0) == 0);
        std::optional<Endpoint> local;
        CHECK(sock::LocalEndpoint(listen_.get(), local) == 0 && local);
        ep_ = *local;
        for (int i = 0; i < 4; ++i) // 队列长度 backlog+1：多连几条，保证满
        {
            Fd f;
            CHECK(sock::Create(ep_, f) == 0);
            (void)sock::Connect(f.get(), ep_);
            fillers_.push_back(std::move(f));
        }
        std::this_thread::sleep_for(100ms);
    }
    const Endpoint& endpoint() const { return ep_; }

    // 新连接确实挂住了（内核行为因环境而异，不满足就跳过相关用例）。
    bool Hangs() const
    {
        Fd f;
        CHECK(sock::Create(ep_, f) == 0);
        const int rc = sock::Connect(f.get(), ep_);
        if (rc != EINPROGRESS)
            return false;
        pollfd p{f.get(), POLLOUT, 0};
        return ::poll(&p, 1, 300) == 0;
    }

private:
    Fd listen_;
    Endpoint ep_ = *Endpoint::Tcp("127.0.0.1", 1);
    std::vector<Fd> fillers_;
};

void TestConnectTimeout()
{
    const LeakCheck leak;
    BlackHole hole;
    if (!hole.Hangs())
    {
        std::puts("net_client_test: 本环境的内核没有把满队列的 SYN 丢掉，跳过 TestConnectTimeout");
        return;
    }
    ClientSide cs;
    FailureLog fl;
    ClientOptions co;
    co.connect_timeout = 150ms;
    co.on_connect_failed = fl.Fn();
    NetClient c(cs.group, hole.endpoint(), std::move(co), FastPolicy(), cs.rec, cs.factory);
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(c.Start() == 0);
    CHECK(WaitUntil([&] { return fl.size() >= 2; }, 5s));
    CHECK(std::chrono::steady_clock::now() - t0 >= 250ms); // 两次超时，每次至少 150ms
    for (const auto& [err, retry] : fl.all())
        CHECK(err == ETIMEDOUT && retry);
    c.Stop();
}

// ---- Stop / Start ----

void TestStopClosesConnectionOnceAndSilences()
{
    const LeakCheck leak;
    EchoServer srv(*Endpoint::Tcp("127.0.0.1", 0));
    ClientSide cs;
    FailureLog fl;
    ClientOptions co;
    co.on_connect_failed = fl.Fn();
    NetClient c(cs.group, srv.server.endpoint(), std::move(co), FastPolicy(), cs.rec, cs.factory);
    c.Stop(); // Idle：空操作
    CHECK(c.Start() == 0);
    const ConnectionHandle h = cs.rec.WaitConnected(0);

    c.Stop(); // 异步：投递给所属 loop，不等待
    CHECK(WaitUntil([&] { return c.handle().raw() == 0; }));
    CHECK(c.Send(Buf("x")) == SendResult::Closed);
    const auto closed = cs.rec.WaitClosed(0);
    CHECK(closed.handle == h && closed.reason == CloseReason::LocalClose);
    CHECK(WaitUntil([&] { return srv.rec.closed_count() == 1; }));
    c.Stop(); // 重复

    std::this_thread::sleep_for(300ms); // 远超退避：不重连、没有任何回调
    CHECK(cs.rec.connected_count() == 1 && cs.rec.closed_count() == 1);
    CHECK(srv.rec.connected_count() == 1);
    CHECK(fl.size() == 0);
    cs.rec.CheckInvariants();
}

void TestStopDuringBackoffCancelsRetry()
{
    const LeakCheck leak;
    ClientSide cs;
    FailureLog fl;
    ClientOptions co;
    co.on_connect_failed = fl.Fn();
    NetClient c(cs.group, FreeTcpEndpoint(), std::move(co), FastPolicy(), cs.rec, cs.factory);
    CHECK(c.Start() == 0);
    CHECK(WaitUntil([&] { return fl.size() >= 2; }));
    c.Stop();
    std::this_thread::sleep_for(50ms); // 让已经在途的那一次尝试结束
    const std::size_t n = fl.size();
    std::this_thread::sleep_for(400ms);
    CHECK(fl.size() == n); // Stop 之后不再尝试，也不再回调
}

// 先 Stop 再 Start：旧连接的 OnClosed 先于新连接的 OnConnected，不会并存两条连接。
// UDS 的 connect 是同步完成的，Start 当场就连上；TCP 要多等一轮 epoll。只测 TCP 会被这点掩盖，所以两种都跑。
void TestStopThenStartNeverOverlaps(const Endpoint& ep)
{
    const LeakCheck leak;
    EchoServer srv(ep);
    std::mutex mu;         // 被钩子引用：要比 cs 活得久（cs 的 IO 线程析构时才停）
    std::vector<char> log; // 全局事件顺序：C = OnConnected，X = OnClosed
    ClientSide cs;
    NetClient c(cs.group, srv.server.endpoint(), {}, FastPolicy(), cs.rec, cs.factory);
    cs.rec.on_connected = [&](Connection&)
    {
        const std::lock_guard lk(mu);
        log.push_back('C');
    };
    cs.rec.on_closed = [&](Connection&, CloseReason)
    {
        const std::lock_guard lk(mu);
        log.push_back('X');
    };

    CHECK(c.Start() == 0);
    cs.rec.WaitConnected(0);
    for (int round = 0; round < 20; ++round) // 反复，覆盖 Stop 与 Start 落在同一轮 / 相邻轮的各种时序
    {
        c.Stop();
        CHECK(c.Start() == 0);
        CHECK(cs.rec.WaitConnected(static_cast<std::size_t>(round) + 1).valid());
    }
    c.Stop();
    CHECK(cs.rec.WaitClosed(20).handle.valid());
    CHECK(WaitUntil(
        [&]
        {
            const std::lock_guard lk(mu);
            return log.size() == 42;
        }));
    {
        const std::lock_guard lk(mu);
        for (std::size_t i = 0; i < log.size(); ++i)
            CHECK(log[i] == (i % 2 == 0 ? 'C' : 'X')); // C X C X ...：严格交替
    }
    CHECK(srv.rec.connected_count() >= 21);
    cs.rec.CheckInvariants();
}

// ---- 不可重试错误 ----

// 在降权的子进程里连一个进不去的路径（EACCES）：状态回到 Idle，回调 will_retry=false，回调里 Start()
// 合法，此后不再尝试。 子进程：fork 之后立刻 exec 本程序，降权发生在新进程的 main 里（exec 之前降权就读不到本程序了）。
int RunFatalErrorChild(const char* path)
{
    if (::geteuid() == 0)
    {
        if (::setgroups(0, nullptr) != 0 || ::setgid(65534) != 0 || ::setuid(65534) != 0)
            return 77; // 降不了权：父进程当作跳过
    }
    const Endpoint ep = *Endpoint::Unix(path);
    ClientSide cs;
    std::atomic<int> calls{0};
    std::atomic<bool> bad{false};
    std::atomic<NetClient*> self{nullptr};
    ClientOptions co;
    co.on_connect_failed = [&](int err, bool retry) noexcept
    {
        if (err != EACCES || retry)
            bad = true;
        if (calls.fetch_add(1) == 0)
            CHECK(self.load()->Start() == 0); // 状态已回到 Idle：回调里直接 Start() 合法
    };
    NetClient c(cs.group, ep, std::move(co), FastPolicy(), cs.rec, cs.factory);
    self = &c;
    CHECK(c.Start() == 0);
    CHECK(WaitUntil([&] { return calls.load() >= 2; }));
    std::this_thread::sleep_for(400ms); // 之后不再尝试
    CHECK(calls.load() == 2 && !bad.load());
    CHECK(cs.rec.connected_count() == 0);
    CHECK(c.Start() == 0); // 回到 Idle 之后可以显式重来
    CHECK(WaitUntil([&] { return calls.load() >= 3; }));
    return 0;
}

void TestFatalErrorGoesIdle()
{
    TempDir dir;
    const std::string priv = dir.path() + "/priv";
    CHECK(::mkdir(priv.c_str(), 0) == 0); // 模式 0：非 root 进不去；root 靠降权
    const std::string sock_path = priv + "/s.sock";

    const pid_t pid = ::fork();
    CHECK(pid >= 0);
    if (pid == 0)
    {
        ::execl(
            "/proc/self/exe", "net_client_test", "--fatal-error-child", sock_path.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
    }
    int status = 0;
    CHECK(::waitpid(pid, &status, 0) == pid);
    ::rmdir(priv.c_str());
    CHECK(WIFEXITED(status));
    if (WEXITSTATUS(status) == 77)
    {
        std::puts("net_client_test: 无法降权，跳过 TestFatalErrorGoesIdle");
        return;
    }
    CHECK(WEXITSTATUS(status) == 0);
}

// ---- 停机与析构 ----

void TestGroupShutdownStopsClient()
{
    const LeakCheck leak;
    EchoServer srv(*Endpoint::Tcp("127.0.0.1", 0));
    FailureLog fl;
    EventRecorder rec;
    LengthPrefixCodecFactory factory(kMaxFrame);
    EventLoopGroup group({1, 8});
    CHECK(group.Start() == 0);
    ClientOptions co;
    co.on_connect_failed = fl.Fn();
    NetClient c(group, srv.server.endpoint(), std::move(co), FastPolicy(), rec, factory);
    CHECK(c.Start() == 0);
    rec.WaitConnected(0);

    group.Shutdown(1000ms);
    // Shutdown 返回时：OnClosed 已回调，且本 client 之后不再有任何回调、不再重连
    CHECK(rec.closed_count() == 1);
    CHECK(rec.WaitClosed(0).reason == CloseReason::LocalClose);
    const std::size_t server_conns = srv.rec.connected_count();
    std::this_thread::sleep_for(300ms);
    CHECK(srv.rec.connected_count() == server_conns && fl.size() == 0 && rec.connected_count() == 1);
    CHECK(c.Send(Buf("x")) == SendResult::Closed);
    rec.CheckInvariants();
}

// 析构时 group 仍在运行、连接还在：同步停掉，OnClosed 在析构返回之前回调。
void TestDestructorWhileConnected()
{
    const LeakCheck leak;
    EchoServer srv(*Endpoint::Tcp("127.0.0.1", 0));
    ClientSide cs;
    {
        NetClient c(cs.group, srv.server.endpoint(), {}, FastPolicy(), cs.rec, cs.factory);
        CHECK(c.Start() == 0);
        cs.rec.WaitConnected(0);
    }
    CHECK(cs.rec.closed_count() == 1);
    CHECK(WaitUntil([&] { return srv.rec.closed_count() == 1; }));
    std::this_thread::sleep_for(150ms); // 不重连
    CHECK(srv.rec.connected_count() == 1);
}

void TestDestructorWhileRetrying()
{
    const LeakCheck leak;
    ClientSide cs;
    FailureLog fl;
    {
        ClientOptions co;
        co.on_connect_failed = fl.Fn();
        NetClient c(cs.group, FreeTcpEndpoint(), std::move(co), FastPolicy(), cs.rec, cs.factory);
        CHECK(c.Start() == 0);
        CHECK(WaitUntil([&] { return fl.size() >= 1; }));
    }
    const std::size_t n = fl.size();
    std::this_thread::sleep_for(300ms);
    CHECK(fl.size() == n); // 定时器与 fd 都随析构清掉了
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 3 && std::strcmp(argv[1], "--fatal-error-child") == 0)
        return RunFatalErrorChild(argv[2]);

    TestBackoffSequence();
    TestPolicyValidity();
    TestInvalidOptionsReturnEinval();
    TestSendBeforeConnectedIsClosed();
    {
        TempDir dir;
        TestConnectSendReceive(*Endpoint::Tcp("127.0.0.1", 0));
        TestConnectSendReceive(dir.Unix("c.sock"));
    }
    TestReconnectAfterPeerCloses();
    TestConnectFailureObservableThenConnects();
    TestUdsSidecarRestart();
    TestConnectTimeout();
    TestStopClosesConnectionOnceAndSilences();
    TestStopDuringBackoffCancelsRetry();
    {
        TempDir dir;
        TestStopThenStartNeverOverlaps(*Endpoint::Tcp("127.0.0.1", 0));
        TestStopThenStartNeverOverlaps(dir.Unix("o.sock"));
    }
    TestFatalErrorGoesIdle();
    TestGroupShutdownStopsClient();
    TestDestructorWhileConnected();
    TestDestructorWhileRetrying();
    std::puts("net_client_test: ok");
    return 0;
}
