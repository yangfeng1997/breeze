// 单 IO 线程 echo 基线（设计见 docs/网络库设计.md §9）。
//
// 被测实现，同一个压测客户端、同一套线格式（4 字节大端长度 + payload）：
//   raw    裸 epoll echo：性能上限，没有任何抽象
//   raw-iov  诊断用：同 raw，但系统调用形状与 breeze 相同（readv 2 个 iovec + sendmsg 2 个 iovec）
//   muduo  muduo 的 TcpServer echo（只支持 TCP）
//   breeze 本库的 IConnectionHandler 直接回调（LoopServer + LengthPrefixCodec），echo 用视图版 Send，不拷贝（与 muduo
//   同一方案） breeze-copy  同 breeze，但 echo 先拷成 SharedBuffer 再发（旧写法，留作参照） breeze-fixed  诊断用：同
//   breeze，但回复预先建好的固定 payload（不分配、不拷贝），用来把框架开销与 echo 里的拷贝分开
//   breeze-mainloop  本库 + framework/net（§9 第 4 组）：真实的 App::Run 主循环 + NetModule（1 个 IO 线程），IO
//   线程把事件 回投主循环，业务在主循环线程上经 ConnectionHandle::Send 回显。与 breeze 的差就是 MainLoopExecutor
//   的代价：拷成 SharedBuffer、事件入队、唤醒主循环、两次线程跳转、回包经任务队列
// raw 与 muduo 把收到的字节原样写回；breeze 先分帧、再把 payload 拷成 SharedBuffer 发回——这是框架多出来的真实成本，
// 所以对比里 breeze 比 raw / muduo 多做了分帧与一次拷贝。
//
// UDP（--transport udp，rudp 的基线）：客户端是同一个闭环 ping-pong，payload 不带 4 字节长度（UDP 自带边界）：
//   raw-udp       裸 UDP 回显：一个 socket、recvmmsg 批量收、sendto 回。没有会话、没有可靠性，是 UDP 的上限
//   rudp          本库 RUdpServer（1 个 IO 线程，echo 用视图版 Send），flush_on_send = true：每条回复立刻发出，
//                 低延迟配置，闭环 ping-pong 的合理选择
//   rudp-batched  同 rudp，但用默认的批量配置（flush_on_send = false，回复等下一次 10ms 的 KCP 定时器才发）：
//                 闭环 ping-pong 下每条连接被钳在 ~100 msg/s，只在连接多时才有吞吐，
//                 留作"默认配置不适合这种负载"的参照
//   rudp-mainloop 同 rudp，但像 breeze-mainloop 一样经 App::Run 主循环 + NetModule（udp://）：IO 线程把事件回投主循环，
//                 业务在主循环线程上经 ConnectionHandle::Send 回显。与 rudp 的差就是 MainLoopExecutor 的代价
//   压测客户端：raw-udp 的每条连接是一个 connect 过的 UDP socket；
//   rudp 的每条连接多一个 KcpSession（握手用服务端签发的票据），每条消息 Update + Send + Flush 立刻发出。
//   不显式 bind：本机内核上 bind(端口 0) 每次约 10ms 且随数量线性变慢，只 connect 让内核自动绑定则是瞬时的
//   （临时端口范围约 28k，够 10k 连接）。丢包（回环上只在服务端接收缓冲溢出时才会）
//   靠 100ms 无回包后重发（raw-udp）/ 驱动 KCP 重传（rudp）兜底。
// TLS / WebSocket / wss（--transport tls|ws|wss，只支持 --impl breeze）：服务端是 production 的 StreamServer（1 个 IO
// 线程）
// + TLS 层 / WebSocket codec；客户端是本库的 StreamClient（每条连接一个，回包在 IO 线程里直接发下一条）。
// 证书在进程内现造（ECDSA P-256，主机名 server.test），客户端验同一个 CA。数字里含 TLS 记录层 / WebSocket 分帧 +
// StreamServer 组管理的成本，与 breeze（裸 LoopServer + LengthPrefix）对照看。这三种传输不支持 --mem-probe
// （每条服务端 TLS 连接的内存见 tls_layer_test 的 TestIdleServerLayerMemory）。
// 内存探测：--mem-probe N（--impl 任意）：建 N 条空闲连接，报告服务端进程 RSS 的增量 / N（见 MemProbe）。
//
// 压测方式：每条连接闭环 ping-pong（发一帧、等完整回包、再发下一帧），预热后计时。
// 用法：net_bench --impl raw|raw-iov|muduo|breeze|breeze-copy|breeze-fixed|breeze-mainloop --transport
// tcp|uds|tls|ws|wss
// --size 64 --conns 64
//                 [--duration 3] [--warmup 1] [--client-threads 4] [--server-cpu 2] [--client-cpus 4,6,8,10]
//                 [--mainloop-cpu 12]   breeze-mainloop / rudp-mainloop 的主循环线程绑哪个核（IO 线程用 --server-cpu）
//                 [--active K]          共 --conns 条连接，只让其中 K 条 ping-pong，其余连上后空闲
//                 [--delay-ms D]        UDP：压测客户端给收和发各加 D 毫秒延迟（往返 2D），模拟有 RTT 的网络。
//                                       本机没有 netem 的权限，只能在客户端里排队；闭环下负载由往返时间决定，
//                                       所以可以在相同负载下比服务端的 CPU
//                 [--rate R]            每条连接每秒发 R 条（默认 0 = 闭环：回包一到就发下一条）。回包后等到
//                                       上一次发出之后 1/R 秒再发，各连接的起点在一个周期内均匀错开。负载由
//                                       连接数 × R 决定、与服务端快慢无关，延迟与服务端 CPU 才有意义
//                                       （闭环全速时延迟只是排队）
//                 [--mem-probe N]       不压测，只测建 N 条空闲连接的内存与空闲 CPU
//                 [--storm N]           重连风暴：N 个客户端线程不停 connect → RST 断开，量每次 accept 的服务端 CPU 与
//                                       跨 loop 投递闭包的分配占比（--io-threads 是服务端 loop 数，默认 4）
//                 [--fanout K]          breeze-mainloop：主循环对同一连接连续 Send K 条回复（量批内合并）
//                 [--reuse-port true|false]       rudp 服务端的 SO_REUSEPORT，默认 false
//                 [--io-threads N]                rudp 的 group 大小（默认 1）；storm 模式默认 4

#include <poll.h>
#include <sched.h>
#include <sys/epoll.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <future>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "base/net/codec/length_prefix_codec.h"
#include "base/net/connection/connection_interface.h"
#include "base/net/endpoint/peer_info.h"
#include "base/net/loop/event_loop_group.h"
#include "base/net/loop/event_loop_thread.h"
#include "base/net/loop/group_access.h"
#include "base/net/rudp/internal/kcp_session.h"
#include "base/net/rudp/rudp_protocol.h"
#include "base/net/rudp/rudp_server.h"
#include "base/net/socket/socket_address.h"
#include "base/net/stream/internal/loop_server.h"
#include "base/net/stream/internal/stream_socket.h"
#include "base/net/stream/stream_client.h"
#include "base/net/stream/stream_server.h"
#include "base/net/tcp/tcp_socket.h"
#if !defined(BREEZE_BENCH_NO_TLS)
#include "base/net/testing/tls_test_util.h"
#include "base/net/tls/tls_context.h"
#include "base/net/ws/ws_codec.h"
#endif
#include "framework/app/app.h"
#include "framework/net/net_module.h"

#ifdef BREEZE_BENCH_MUDUO
#include <muduo/base/Logging.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/TcpServer.h>
#endif

using namespace breeze;
using namespace breeze::testing; // MakeCert / TestCert（bench 自己现造 TLS 测试证书）

namespace
{

using BenchClock = std::chrono::steady_clock; // 不叫 Clock：framework 的 module.h 里有 breeze::Clock，会二义

[[noreturn]] void Die(const char* what)
{
    std::fprintf(stderr, "net_bench: %s\n", what);
    std::exit(2);
}
#define REQUIRE(cond)                                                                                                  \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
            Die(#cond);                                                                                                \
    } while (0)

void PinTo(int cpu)
{
    if (cpu < 0)
        return;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    sched_setaffinity(0, sizeof(set), &set); // 绑定调用线程
}

// breeze-mainloop 的扇出：主循环对**同一条连接**连续 Send 这么多条回复（客户端等齐 K 条才算一次往返）。
// 1 = 普通回显。用来量"批内连续 Send"的合并收益（跨线程 Send 各自一次 sendmsg，还是批末合成一次）。
int g_fanout = 1;
int g_storm_threads = 0; // > 0：重连风暴模式（--storm N：N 个客户端线程不停 connect → RST 断开），见 Storm()
int g_io_threads = 4;    // 重连风暴里服务端的 loop 数

struct BenchOptions
{
    std::string impl = "breeze";
    std::string transport = "tcp";
    std::size_t size = 64;
    int conns = 64;
    int duration_s = 3;
    int warmup_s = 1;
    int client_threads = 4;
    int server_cpu = -1;
    int mainloop_cpu = -1;
    int mem_probe = 0;
    int delay_ms = 0; // UDP：客户端侧给每个方向各加这么多毫秒的延迟（往返 = 2 倍），模拟有 RTT 的网络
    int active = 0;   // > 0：只让这么多条连接 ping-pong，其余连上之后空闲（测「会话多」对活跃连接的影响）
    double rate = 0;  // > 0：每条连接每秒发这么多条（定速负载）；0 = 闭环全速
    bool reuse_port = false;
    int io_threads = 1; // rudp：group 大小；reuse_port 关闭时只在一个 loop 收包
    std::vector<int> client_cpus;
};

bool ParseBool(const std::string& s)
{
    if (s == "true")
        return true;
    if (s == "false")
        return false;
    Die("--reuse-port 只能是 true 或 false");
}

std::uint64_t GroupIterations(EventLoopGroup& group)
{
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < GroupAccess::LoopCount(group); ++i)
        total += GroupAccess::LoopAt(group, i).Iterations();
    return total;
}

std::vector<pthread_t> PinGroup(EventLoopGroup& group, int cpu)
{
    std::vector<pthread_t> threads;
    for (std::size_t i = 0; i < GroupAccess::LoopCount(group); ++i)
    {
        std::promise<pthread_t> ready;
        auto future = ready.get_future();
        GroupAccess::LoopAt(group, i).Post(
            [cpu, &ready]
            {
                // --server-cpu 只指定一个核时，所有服务端 IO 线程共用它。
                PinTo(cpu);
                ready.set_value(::pthread_self());
            });
        threads.push_back(future.get());
    }
    return threads;
}

std::vector<int> ParseCpus(const std::string& s)
{
    std::vector<int> v;
    std::size_t i = 0;
    while (i < s.size())
    {
        const std::size_t j = s.find(',', i);
        v.push_back(std::atoi(s.substr(i, j == std::string::npos ? j : j - i).c_str()));
        if (j == std::string::npos)
            break;
        i = j + 1;
    }
    return v;
}

// 服务端连接表的槽数（每槽 64 字节）：默认 64K，连接数更多时放大到 128K；main 在建服务端之前按 --conns 设定。
std::uint32_t g_table_slots = 1u << 16;

// TCP 连接数超过 2 万时，单个（源地址, 目的地址）的临时端口不够（本机范围 32768–60999，约 2.8 万）：
// 把连接分散到 127.0.0.1..8（服务端监听 0.0.0.0，源地址随目的地址，四元组各不相同）。
// UDP 做不到 2.8 万以上：服务端按来源 ip:port 固定会话，每条连接要一个独一无二的来源地址，
// 而内核自动选端口只有约 2.8 万个（SO_REUSEADDR 会让端口重复、来源地址冲突；回环上源地址永远是 127.0.0.1，
// 改目的地址也没用），显式 bind 在本机每次约 10ms 且全局串行（多线程不重叠，换 IPv6 / SO_REUSEPORT 都一样），
// 10 万条要 1000 秒以上。
constexpr int kScaleThreshold = 20000;
constexpr int kScaleAddrs = 8;
constexpr int kUdpMaxConns = 25000;

Endpoint ScaleTcpTarget(const Endpoint& ep, int global_index)
{
    return *Endpoint::Tcp("127.0.0." + std::to_string(1 + global_index % kScaleAddrs), ep.port());
}

// 压测客户端的等待：纳秒精度（epoll_pwait2）并把线程的定时器松弛降到 1ns。定速时每个发送都在各自的到期时刻发出，
// 别按毫秒取整：取整会让一个毫秒里到期的发送攒成一个突发（回环上服务端的接收缓冲被一次打爆，延迟里混进排队），
// 实际速率也会因为每次晚一点而偏低（每条连接每秒 100 条时低了约 6%）。
int EpollWaitFor(int epfd, epoll_event* evs, int max, std::chrono::nanoseconds timeout)
{
    timeout = std::max(timeout, std::chrono::nanoseconds(0));
    const timespec ts{static_cast<time_t>(timeout.count() / 1000000000),
                      static_cast<long>(timeout.count() % 1000000000)};
    return ::epoll_pwait2(epfd, evs, max, &ts, nullptr);
}

// ---- 被测服务端 ----

class EchoServer
{
public:
    virtual ~EchoServer() = default;
    // 监听 ep（端口可为 0），返回实际绑定的地址；返回时已经可以连接。
    virtual Endpoint Start(const Endpoint& ep, int cpu) = 0;
    virtual void Stop() = 0;
    // 服务端 IO 线程：读它的 CPU 时间，判断压测时它是否已经饱和（没饱和说明瓶颈在别处，比出来的不是服务端的差异）。
    virtual pthread_t ServerThread() = 0;
    virtual std::vector<pthread_t> ServerThreads() { return {ServerThread()}; }
    virtual std::uint64_t Connections() { return 0; }
    // 服务端 loop 已经跑过的轮数；没有"轮"概念的实现返回 0。
    virtual std::uint64_t Iterations() { return 0; }
    // 除 IO 线程之外还有第二个服务端线程（主循环）的实现返回它，否则 0：同样读它的 CPU 时间，看瓶颈在哪一个。
    virtual pthread_t ExtraThread() { return 0; }
    // rudp 的服务端要凭票据握手：签发 n 张；其它实现返回空。
    virtual std::vector<RUdpTicket> IssueTickets(int /*n*/) { return {}; }
    // 服务端 loop 迄今为止单轮（分发事件 + 任务 + 定时器 + 回收）最长的耗时（微秒）；没有这个概念的实现返回 0。
    virtual double MaxLoopLagUs() { return 0; }
    // 服务端 loop 迄今收到的字节数（rudp 的 UdpSocket 统计）；没有这个统计的实现返回 0。
    virtual std::uint64_t BytesIn() { return 0; }
};

// 裸 epoll：水平触发，读多少原样写回多少。
//
// 诊断选项（二分实验用：往 raw-iov 上一次加一个 breeze 的特征，看哪个让吞吐掉到 breeze 的水平）：
//   iov    读用 readv（2 个 iovec）、写用 sendmsg（把回包从第 4 字节处切成 2 个 iovec，等同"帧头 + payload"）——与
//   breeze 同形状的系统调用 rdhup  epoll 注册带 EPOLLRDHUP（breeze 有） cold   每连接一个 2KB 读缓冲，readv
//   读进它、回包从它发出（breeze 的每连接 Buffer）；否则所有连接共用一块 64KB ptr    epoll 的 data
//   存指向每连接对象的指针，而不是 fd（breeze 存 IPollable*）
struct RawOptions
{
    bool iov = false;
    bool rdhup = false;
    bool cold = false;
    bool ptr = false;
};

class RawServer final : public EchoServer
{
public:
    explicit RawServer(RawOptions opt = {})
        : opt_(opt)
    {
    }

    Endpoint Start(const Endpoint& ep, int cpu) override
    {
        UniqueFd lfd;
        REQUIRE(sock::CreateStreamSocket(ep, lfd) == 0);
        if (ep.kind() == Endpoint::Kind::Tcp)
            REQUIRE(tcp::SetReuseAddr(lfd.get()) == 0);
        REQUIRE(sock::Bind(lfd.get(), ep) == 0);
        REQUIRE(sock::Listen(lfd.get(), 4096) == 0);
        std::optional<Endpoint> local;
        REQUIRE(sock::LocalEndpoint(lfd.get(), local) == 0 && local);
        tcp_ = ep.kind() == Endpoint::Kind::Tcp;
        lfd_ = std::move(lfd);
        epfd_ = UniqueFd(::epoll_create1(EPOLL_CLOEXEC));
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.ptr = nullptr; // 监听 fd 用空指针标记
        REQUIRE(::epoll_ctl(epfd_.get(), EPOLL_CTL_ADD, lfd_.get(), &ev) == 0);
        thread_ = std::thread([this, cpu] { Loop(cpu); });
        return *local;
    }

    void Stop() override
    {
        stop_ = true;
        thread_.join();
    }
    pthread_t ServerThread() override { return thread_.native_handle(); }

private:
    struct Conn
    {
        UniqueFd fd;
        std::unique_ptr<std::byte[]> buf; // cold 模式下的每连接读缓冲
    };
    static constexpr std::size_t kColdCap = 2048;

    void Loop(int cpu)
    {
        PinTo(cpu);
        std::vector<std::byte> shared(64 * 1024);
        std::vector<epoll_event> evs(1024);
        std::vector<std::unique_ptr<Conn>> by_fd; // 按 fd 号索引，同时负责在退出时关闭
        while (!stop_)
        {
            const int n = ::epoll_wait(epfd_.get(), evs.data(), static_cast<int>(evs.size()), 100);
            for (int i = 0; i < n; ++i)
            {
                void* const tag = evs[static_cast<std::size_t>(i)].data.ptr;
                if (tag == nullptr) // 监听 fd
                {
                    for (int k = 0; k < 64; ++k)
                    {
                        UniqueFd c;
                        std::optional<Endpoint> peer;
                        if (sock::Accept(lfd_.get(), c, peer) != 0)
                            break;
                        if (tcp_)
                            (void)tcp::SetNoDelay(c.get());
                        auto conn = std::make_unique<Conn>();
                        if (opt_.cold)
                            conn->buf = std::make_unique_for_overwrite<std::byte[]>(kColdCap);
                        epoll_event ev{};
                        ev.events = EPOLLIN | (opt_.rdhup ? EPOLLRDHUP : 0);
                        // ptr 模式存对象指针；否则存 fd 号（+1，避免与监听 fd 的空指针标记混淆）
                        ev.data.ptr = opt_.ptr ? static_cast<void*>(conn.get())
                                               : reinterpret_cast<void*>(static_cast<std::intptr_t>(c.get()) + 1);
                        REQUIRE(::epoll_ctl(epfd_.get(), EPOLL_CTL_ADD, c.get(), &ev) == 0);
                        const auto idx = static_cast<std::size_t>(c.get());
                        if (by_fd.size() <= idx)
                            by_fd.resize(idx + 1);
                        conn->fd = std::move(c);
                        by_fd[idx] = std::move(conn);
                    }
                    continue;
                }
                Conn* conn = opt_.ptr ? static_cast<Conn*>(tag)
                                      : by_fd[static_cast<std::size_t>(reinterpret_cast<std::intptr_t>(tag) - 1)].get();
                const int fd = conn->fd.get();
                std::byte* data = opt_.cold ? conn->buf.get() : shared.data();
                const std::size_t cap = opt_.cold ? kColdCap : shared.size();

                ssize_t r;
                if (opt_.iov)
                {
                    // 第一段是缓冲头部，第二段接着往后（cold 时落在共享缓冲里，只为形状一致；基准消息
                    // ≤1028B，装得进第一段）
                    iovec iv[2] = {{data, opt_.cold ? kColdCap : 1024},
                                   opt_.cold ? iovec{shared.data(), shared.size()}
                                             : iovec{shared.data() + 1024, shared.size() - 1024}};
                    r = ::readv(fd, iv, 2);
                }
                else
                {
                    r = ::read(fd, data, cap);
                }
                if (r <= 0)
                {
                    if (r < 0 && (errno == EAGAIN || errno == EINTR))
                        continue;
                    ::epoll_ctl(epfd_.get(), EPOLL_CTL_DEL, fd, nullptr);
                    continue; // fd 留在 by_fd 里，退出时关闭
                }
                // 读到第二段的部分（cold 且消息超过 kColdCap 时）不在本诊断变体的范围内
                const std::size_t got = static_cast<std::size_t>(r);
                std::size_t off = 0;
                while (off < got)
                {
                    ssize_t w;
                    if (opt_.iov)
                    {
                        const std::size_t left = got - off;
                        const std::size_t first = left > 4 ? 4 : left;
                        iovec iv[2] = {{data + off, first}, {data + off + first, left - first}};
                        msghdr mh{};
                        mh.msg_iov = iv;
                        mh.msg_iovlen = left > first ? 2 : 1;
                        w = ::sendmsg(fd, &mh, MSG_NOSIGNAL);
                    }
                    else
                    {
                        w = ::send(fd, data + off, got - off, MSG_NOSIGNAL);
                    }
                    if (w > 0)
                        off += static_cast<std::size_t>(w);
                    else if (w < 0 && errno == EAGAIN)
                    {
                        pollfd p{fd, POLLOUT, 0};
                        ::poll(&p, 1, 100);
                    }
                    else
                        break;
                }
            }
        }
    }

    RawOptions opt_;
    UniqueFd lfd_;
    UniqueFd epfd_;
    bool tcp_ = true;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

// 本库：LoopServer + LengthPrefixCodec，回调里用视图版 Send 把 payload 直接从读缓冲写回——不分配、不拷贝，
// 与 muduo 的 echo（send(Buffer*) 直接交换数据）是同一方案。
class BenchHandler final : public IConnectionHandler
{
public:
    void OnMessage(IConnection& c, std::span<const std::byte> payload) noexcept override { (void)c.Send(payload); }
};

// 旧写法留作参照（breeze-copy）：先拷成 SharedBuffer 再发，每条消息多一次分配与拷贝。
class CopyEchoHandler final : public IConnectionHandler
{
public:
    void OnMessage(IConnection& c, std::span<const std::byte> payload) noexcept override
    {
        (void)c.Send(SharedBuffer::CopyFrom(payload));
    }
};

// 诊断变体：回复一块预先建好的固定 payload（压测客户端每次发的内容都相同），不分配、不拷贝。
// 与 breeze 的差就是 echo 里"把收到的 payload 拷成 SharedBuffer"的成本；与 raw
// 的差才是框架本身（分帧、事件分发、帧头）。
class FixedReplyHandler final : public IConnectionHandler
{
public:
    explicit FixedReplyHandler(std::size_t size)
    {
        reply_ = SharedBuffer::Create(size);
        for (std::size_t i = 0; i < size; ++i)
            reply_.MutableBytes()[i] = static_cast<std::byte>('a' + i % 26);
    }
    void OnMessage(IConnection& c, std::span<const std::byte>) noexcept override { (void)c.Send(reply_); }

private:
    SharedBuffer reply_;
};

class BreezeServer final : public EchoServer
{
public:
    explicit BreezeServer(std::unique_ptr<IConnectionHandler> handler)
        : handler_(std::move(handler))
    {
    }

    Endpoint Start(const Endpoint& ep, int cpu) override
    {
        factory_ = std::make_unique<LengthPrefixCodecFactory>(1024 * 1024);
        srv_ = std::make_unique<LoopServer>(lt_.loop, ep, *handler_, *factory_);
        REQUIRE(srv_->Start() == 0);
        lt_.Start();
        if (cpu >= 0)
            lt_.loop.Post([cpu] { PinTo(cpu); });
        return srv_->endpoint();
    }
    void Stop() override { lt_.Stop(); }
    pthread_t ServerThread() override { return lt_.native_handle(); }
    std::uint64_t Connections() override { return lt_.loop.ConnectionCount(); }
    std::uint64_t Iterations() override { return lt_.loop.Iterations(); }

private:
    std::unique_ptr<IConnectionHandler> handler_;
    EventLoopThread lt_{EventLoopOptions{EventLoopOptions::kNoGroup, 0, g_table_slots}}; // 连接表要装得下全部连接
    std::unique_ptr<LengthPrefixCodecFactory> factory_;
    std::unique_ptr<LoopServer> srv_;
};

#if !defined(BREEZE_BENCH_NO_TLS)
// ---- TLS / WebSocket / wss（--transport tls|ws|wss）----
//
// 与 breeze 的 bare LoopServer 不同，这几种传输走 production 的 StreamServer（1 个 IO 线程）：
//   tls   TLS 层 + LengthPrefixCodec（线格式与 tcp 相同）
//   ws    WebSocket codec（一条消息一帧，不分片）
//   wss   TLS + WebSocket codec
// 服务端证书在进程内现造（与 tls_layer_test 同一套 CA，ECDSA P-256，主机名 server.test）；客户端信任同一个 CA
// 并校验证书。 数字里包含 TLS 记录层 / WebSocket 分帧 + StreamServer 组管理的成本，不是 raw 基线；与 breeze
// 的差就是这一层。

struct BenchPki
{
    TestCert ca = MakeCert({.cn = "Bench CA", .is_ca = true});
    TestCert server = MakeCert({.cn = "server.test", .dns = {"server.test"}, .issuer = &ca});
};

const BenchPki& BenchCerts()
{
    static const BenchPki pki;
    return pki;
}

TlsServerOptions BenchServerTls()
{
    TlsServerOptions o;
    o.cert_chain.pem = BenchCerts().server.cert_pem;
    o.private_key.pem = BenchCerts().server.key_pem;
    return o;
}

TlsClientOptions BenchClientTls()
{
    TlsClientOptions o;
    o.server_name = "server.test";
    o.ca.pem = BenchCerts().ca.cert_pem;
    return o;
}

// 按原样回显（LengthPrefix 与 WebSocket 都一样：发回 payload）。
class PongHandler final : public IConnectionHandler
{
public:
    void OnMessage(IConnection& c, std::span<const std::byte> payload) noexcept override { (void)c.Send(payload); }
};

class SecureServer final : public EchoServer
{
public:
    SecureServer(bool tls, bool ws)
        : tls_(tls)
        , ws_(ws)
    {
    }

    Endpoint Start(const Endpoint& ep, int cpu) override
    {
        if (ws_)
        {
            WsServerOptions wo;
            wo.message_type = WsMessageType::Binary;
            wo.max_message_bytes = 1 << 20;
            ws_factory_ = std::make_unique<WsServerCodecFactory>(std::move(wo));
        }
        else
        {
            lp_factory_ = std::make_unique<LengthPrefixCodecFactory>(1024 * 1024);
        }
        if (tls_)
        {
            std::string err;
            tls_ctx_ = TlsContext::CreateServer(BenchServerTls(), err);
            REQUIRE(tls_ctx_ != nullptr);
        }
        handler_ = std::make_unique<PongHandler>();
        group_ = std::make_unique<EventLoopGroup>(EventLoopGroupOptions{1, g_table_slots});
        StreamServerOptions so;
        so.connection.high_water_mark = 64u << 20;
        so.tls = tls_ctx_;
        const IFrameCodecFactory& factory = ws_ ? static_cast<const IFrameCodecFactory&>(*ws_factory_) : *lp_factory_;
        server_ = std::make_unique<StreamServer>(*group_, ep, std::move(so), *handler_, factory);
        REQUIRE(group_->Start() == 0);
        REQUIRE(server_->Start() == 0);
        // IO 线程的 pthread 句柄：绑核与读它的 CPU 时间要用（EventLoop 不暴露句柄，从 loop 线程里取）。
        std::promise<pthread_t> ready;
        auto fut = ready.get_future();
        GroupAccess::LoopAt(*group_, 0)
            .Post(
                [cpu, &ready]
                {
                    PinTo(cpu);
                    ready.set_value(::pthread_self());
                });
        thread_ = fut.get();
        return server_->endpoint();
    }
    void Stop() override
    {
        if (server_)
            server_->Stop();
        if (group_)
            group_->Shutdown(std::chrono::milliseconds::zero());
    }
    pthread_t ServerThread() override { return thread_; }
    std::uint64_t Connections() override { return group_->GetStats().connections; }
    std::uint64_t Iterations() override { return group_ ? GroupAccess::LoopAt(*group_, 0).Iterations() : 0; }

private:
    bool tls_;
    bool ws_;
    std::shared_ptr<const IStreamLayerFactory> tls_ctx_;
    std::unique_ptr<PongHandler> handler_;
    std::unique_ptr<LengthPrefixCodecFactory> lp_factory_;
    std::unique_ptr<WsServerCodecFactory> ws_factory_;
    std::unique_ptr<EventLoopGroup> group_;
    std::unique_ptr<StreamServer> server_;
    pthread_t thread_{};
};

#endif // !BREEZE_BENCH_NO_TLS

// 本库 + framework/net：真实的 App::Run 主循环 + NetModule（1 个 IO
// 线程），业务在主循环线程上经句柄把收到的消息原样发回 （SharedBuffer 在 IO
// 线程拷出来、主循环线程直接发回，不再拷）。比 breeze 多出的就是 MainLoopExecutor 的代价。
class MainLoopServer final : public EchoServer
{
    class EchoBiz final : public IMainLoopHandler
    {
    public:
        void OnMessage(ConnectionHandle h, SharedBuffer msg) override
        {
            for (int i = 1; i < g_fanout; ++i)
                (void)h.Send(msg); // 共享同一块字节，只加引用计数
            (void)h.Send(std::move(msg));
        }
    };

    class BizModule final : public Module
    {
    public:
        BizModule(App& a,
                  EchoBiz& biz,
                  const Endpoint& listen,
                  bool rudp,
                  std::string scheme,
                  std::promise<Endpoint>& ready,
                  RUdpServer*& rudp_out)
            : Module(a)
            , biz_(biz)
            , listen_(listen)
            , rudp_(rudp)
            , scheme_(std::move(scheme))
            , ready_(ready)
            , rudp_out_(rudp_out)
        {
        }
        std::string_view Name() const override { return "biz"; }
        std::vector<std::string_view> DependsOn() const override { return {"net"}; }
        std::expected<void, Error> Init() override
        {
            auto* net = app().GetModule<NetModule>("net");
            if (rudp_)
            {
                // 10 万会话时要一次签发 10 万张票据，超过默认的待用票据上限（65536）
                RUdpServerOptions so;
                so.connection = net->options().connection;
                so.rudp = net->options().rudp;
                so.reuse_port = net->options().rudp_reuse_port;
                so.max_pending_tickets = 1u << 18;
                auto s = net->AddRUdpServer(*RUdpEndpoint::From(listen_), biz_, std::move(so));
                if (!s)
                    return std::unexpected(s.error());
                server_ = *s;
                rudp_out_ = *s; // Start 里的 set_value 把它发布给压测线程
                return {};
            }
            // tls / ws / wss：把 tcp:// 换成对应的 scheme（NetModule 按 net.tls 的证书建上下文）；ws / wss
            // 自带分帧，不给 codec 工厂。
            std::string url = listen_.ToString();
            if (scheme_ != "tcp")
                url = scheme_ + url.substr(3);
            auto s =
                (scheme_ == "ws" || scheme_ == "wss") ? net->AddServer(url, biz_) : net->AddServer(url, biz_, factory_);
            if (!s)
                return std::unexpected(s.error());
            server_ = *s;
            return {};
        }
        std::expected<void, Error> Start() override
        {
            ready_.set_value(server_->endpoint());
            return {};
        }

    private:
        EchoBiz& biz_;
        Endpoint listen_;
        bool rudp_;
        std::string scheme_;
        LengthPrefixCodecFactory factory_{1024 * 1024};
        std::promise<Endpoint>& ready_;
        RUdpServer*& rudp_out_;
        INetServer* server_ = nullptr;
    };

public:
    // rudp = true：udp:// 端点（rudp 服务端，flush_on_send），业务照样在主循环线程上回显。
    // scheme：tcp（端点原样用）| tls | ws | wss（tcp 端点换 scheme；证书写到临时目录，经 net.tls 的路径交给
    // NetModule）。
    MainLoopServer(const BenchOptions& options, bool rudp, std::string scheme = "tcp")
        : mainloop_cpu_(options.mainloop_cpu)
        , rudp_(rudp)
        , scheme_(std::move(scheme))
        , reuse_port_(options.reuse_port)
        , io_threads_(rudp ? options.io_threads : 1)
    {
    }

    Endpoint Start(const Endpoint& ep, int cpu) override
    {
        NetModuleOptions no;
        no.group = {static_cast<std::uint32_t>(io_threads_), g_table_slots};
        no.rudp_reuse_port = reuse_port_;
        no.rudp.flush_on_send = true; // 只对 udp:// 生效，与 rudp 一样用低延迟配置
#if !defined(BREEZE_BENCH_NO_TLS)
        if (scheme_ == "tls" || scheme_ == "wss")
        {
            char tmpl[] = "/tmp/breeze_bench_tls_XXXXXX";
            REQUIRE(::mkdtemp(tmpl) != nullptr);
            cert_dir_ = tmpl;
            const auto write = [&](const char* name, const std::string& text)
            {
                const std::string path = cert_dir_ + "/" + name;
                std::FILE* f = std::fopen(path.c_str(), "wb");
                REQUIRE(f != nullptr && std::fwrite(text.data(), 1, text.size(), f) == text.size());
                std::fclose(f);
                return path;
            };
            no.tls.cert_chain_file = write("server.pem", BenchCerts().server.cert_pem);
            no.tls.private_key_file = write("server.key", BenchCerts().server.key_pem);
        }
#endif
        app_ = std::make_unique<App>("bench");
        app_->RegisterModule(
            [&](App& a)
            {
                auto m = std::make_unique<NetModule>(a, no);
                net_ = m.get();
                return m;
            });
        std::promise<Endpoint> ready;
        app_->RegisterModule([&](App& a)
                             { return std::make_unique<BizModule>(a, biz_, ep, rudp_, scheme_, ready, rudp_server_); });
        auto bound = ready.get_future();
        thread_ = std::thread(
            [this]
            {
                PinTo(mainloop_cpu_);
                result_ = app_->Run(stop_.get_token());
                if (!result_)
                    Die(result_.error().msg.c_str());
            });
        const Endpoint actual = bound.get(); // Run 失败时 Die 会退出进程，不会一直等
        io_threads_native_ = PinGroup(net_->group(), cpu);
        return actual;
    }
    void Stop() override
    {
        stop_.request_stop();
        thread_.join();
        // Run 已在主循环线程完成 Stop / Shutdown，此处只释放对象。
        app_.reset();
        if (!cert_dir_.empty())
        {
            ::unlink((cert_dir_ + "/server.pem").c_str());
            ::unlink((cert_dir_ + "/server.key").c_str());
            ::rmdir(cert_dir_.c_str());
        }
    }
    pthread_t ServerThread() override { return io_threads_native_.front(); }
    std::vector<pthread_t> ServerThreads() override { return io_threads_native_; }
    pthread_t ExtraThread() override { return thread_.native_handle(); }
    std::uint64_t Connections() override { return net_->group().GetStats().connections; }
    std::uint64_t Iterations() override { return GroupIterations(net_->group()); }
    std::vector<RUdpTicket> IssueTickets(int n) override
    {
        REQUIRE(rudp_ && rudp_server_ != nullptr);
        std::vector<RUdpTicket> out;
        for (int i = 0; i < n; ++i)
        {
            const auto t = rudp_server_->IssueTicket(std::chrono::minutes(10));
            REQUIRE(t.has_value());
            out.push_back(*t);
        }
        return out;
    }

private:
    int mainloop_cpu_;
    bool rudp_;
    std::string scheme_;
    bool reuse_port_;
    int io_threads_;
    std::string cert_dir_; // tls / wss：证书临时目录（进程退出前由 Stop 清理）
    RUdpServer* rudp_server_ = nullptr;
    EchoBiz biz_;
    std::unique_ptr<App> app_;
    NetModule* net_ = nullptr;
    std::stop_source stop_;
    std::expected<void, Error> result_;
    std::thread thread_;
    std::vector<pthread_t> io_threads_native_;
};

#ifdef BREEZE_BENCH_MUDUO
class MuduoServer final : public EchoServer
{
public:
    Endpoint Start(const Endpoint& ep, int cpu) override
    {
        REQUIRE(ep.kind() == Endpoint::Kind::Tcp); // muduo 只支持 TCP
        // muduo 拿不到系统分配的端口：先探一个空闲端口再交给它。
        std::uint16_t port = ep.port();
        if (port == 0)
        {
            UniqueFd probe;
            REQUIRE(sock::CreateStreamSocket(ep, probe) == 0);
            REQUIRE(sock::Bind(probe.get(), ep) == 0);
            std::optional<Endpoint> local;
            REQUIRE(sock::LocalEndpoint(probe.get(), local) == 0 && local);
            port = local->port();
        }
        muduo::Logger::setLogLevel(muduo::Logger::WARN);
        std::promise<void> ready;
        thread_ = std::thread(
            [this, port, cpu, &ready]
            {
                PinTo(cpu);
                muduo::net::EventLoop loop;
                loop_ = &loop;
                muduo::net::InetAddress addr(port);                 // 任意地址，与另两个实现一致
                muduo::net::TcpServer server(&loop, addr, "bench"); // 默认 0 个工作线程：全在这一个 loop 上
                server.setConnectionCallback(
                    [](const muduo::net::TcpConnectionPtr& c)
                    {
                        if (c->connected())
                            c->setTcpNoDelay(true);
                    });
                server.setMessageCallback(
                    [](const muduo::net::TcpConnectionPtr& c, muduo::net::Buffer* buf, muduo::Timestamp)
                    { c->send(buf); }); // send(Buffer*) 直接交换数据，不多拷一次
                server.start();
                ready.set_value();
                loop.loop();
            });
        ready.get_future().wait();
        return *Endpoint::Tcp("127.0.0.1", port);
    }
    void Stop() override
    {
        loop_->quit();
        thread_.join();
    }
    pthread_t ServerThread() override { return thread_.native_handle(); }

private:
    muduo::net::EventLoop* loop_ = nullptr;
    std::thread thread_;
};
#endif

// 裸 UDP 回显：一个 socket，epoll + recvmmsg 批量收（与本库 UdpSocket 同一个形状），逐个 sendto 回去。
class RawUdpServer final : public EchoServer
{
public:
    Endpoint Start(const Endpoint& ep, int cpu) override
    {
        REQUIRE(sock::CreateSocket(ep, SOCK_DGRAM, fd_) == 0);
        const int buf = 4 << 20;
        ::setsockopt(fd_.get(), SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));
        ::setsockopt(fd_.get(), SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
        sockaddr_storage ss;
        const socklen_t len = sock::ToSockaddr(ep, ss);
        REQUIRE(::bind(fd_.get(), reinterpret_cast<const sockaddr*>(&ss), len) == 0);
        sockaddr_storage got;
        socklen_t gl = sizeof(got);
        REQUIRE(::getsockname(fd_.get(), reinterpret_cast<sockaddr*>(&got), &gl) == 0);
        const auto local = sock::FromSockaddr(got, gl, Endpoint::Kind::Udp);
        REQUIRE(local.has_value());
        epfd_ = UniqueFd(::epoll_create1(EPOLL_CLOEXEC));
        epoll_event ev{};
        ev.events = EPOLLIN;
        REQUIRE(::epoll_ctl(epfd_.get(), EPOLL_CTL_ADD, fd_.get(), &ev) == 0);
        thread_ = std::thread([this, cpu] { Loop(cpu); });
        return *local;
    }
    void Stop() override
    {
        stop_ = true;
        thread_.join();
    }
    pthread_t ServerThread() override { return thread_.native_handle(); }

private:
    static constexpr unsigned kBatch = 16;
    static constexpr std::size_t kSlot = 2048;

    void Loop(int cpu)
    {
        PinTo(cpu);
        static thread_local std::byte bufs[kBatch][kSlot];
        sockaddr_storage addrs[kBatch];
        iovec iov[kBatch];
        mmsghdr msgs[kBatch];
        epoll_event evs[8];
        while (!stop_)
        {
            if (::epoll_wait(epfd_.get(), evs, 8, 100) <= 0)
                continue;
            for (;;)
            {
                for (unsigned i = 0; i < kBatch; ++i)
                {
                    iov[i] = {bufs[i], kSlot};
                    msgs[i] = {};
                    msgs[i].msg_hdr.msg_iov = &iov[i];
                    msgs[i].msg_hdr.msg_iovlen = 1;
                    msgs[i].msg_hdr.msg_name = &addrs[i];
                    msgs[i].msg_hdr.msg_namelen = sizeof(sockaddr_storage);
                }
                const int n = ::recvmmsg(fd_.get(), msgs, kBatch, MSG_DONTWAIT, nullptr);
                if (n <= 0)
                    break;
                for (int i = 0; i < n; ++i)
                    ::sendto(fd_.get(),
                             bufs[i],
                             msgs[i].msg_len,
                             0,
                             reinterpret_cast<sockaddr*>(&addrs[i]),
                             msgs[i].msg_hdr.msg_namelen);
                if (n < static_cast<int>(kBatch))
                    break;
            }
        }
    }

    UniqueFd fd_;
    UniqueFd epfd_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

// 本库 rudp：1 个 IO 线程的 EventLoopGroup + RUdpServer，回调里用视图版 Send 回显（与 breeze 的 TCP 同一个写法）。
class RUdpEchoHandler final : public IConnectionHandler
{
public:
    void OnMessage(IConnection& c, std::span<const std::byte> payload) noexcept override { (void)c.Send(payload); }
};

class RUdpBenchServer final : public EchoServer
{
public:
    RUdpBenchServer(bool flush_on_send, const BenchOptions& options)
        : flush_on_send_(flush_on_send)
        , reuse_port_(options.reuse_port)
        , io_threads_(options.io_threads)
    {
    }

    Endpoint Start(const Endpoint& ep, int cpu) override
    {
        group_ = std::make_unique<EventLoopGroup>(
            EventLoopGroupOptions{static_cast<std::uint32_t>(io_threads_), g_table_slots});
        REQUIRE(group_->Start() == 0);
        RUdpServerOptions so;
        so.reuse_port = reuse_port_;
        so.rudp.flush_on_send = flush_on_send_;
        so.max_pending_tickets = 1u << 18; // 10 万会话时要一次签发 10 万张票据，超过默认的 65536
        server_ = std::make_unique<RUdpServer>(*group_, *RUdpEndpoint::From(ep), std::move(so), handler_);
        REQUIRE(server_->Start() == 0);
        io_threads_native_ = PinGroup(*group_, cpu);
        return server_->endpoint();
    }
    void Stop() override
    {
        // 压测客户端已经全部关闭，但服务端会话还在（rudp 没有 TCP 的 RST）：强关再销毁。
        group_->Shutdown(std::chrono::milliseconds::zero());
        server_.reset();
        group_.reset();
    }
    pthread_t ServerThread() override { return io_threads_native_.front(); }
    std::vector<pthread_t> ServerThreads() override { return io_threads_native_; }
    std::uint64_t Connections() override { return group_->GetStats().connections; }
    std::uint64_t Iterations() override { return GroupIterations(*group_); }
    std::uint64_t BytesIn() override { return group_->GetStats().bytes_in; }
    double MaxLoopLagUs() override
    {
        return static_cast<double>(
            std::chrono::duration_cast<std::chrono::microseconds>(group_->GetStats().max_loop_lag).count());
    }
    std::vector<RUdpTicket> IssueTickets(int n) override
    {
        std::vector<RUdpTicket> out;
        for (int i = 0; i < n; ++i)
        {
            const auto t = server_->IssueTicket(std::chrono::minutes(10));
            REQUIRE(t.has_value());
            out.push_back(*t);
        }
        return out;
    }

private:
    bool flush_on_send_;
    bool reuse_port_;
    int io_threads_;
    RUdpEchoHandler handler_;
    std::unique_ptr<EventLoopGroup> group_;
    std::unique_ptr<RUdpServer> server_;
    std::vector<pthread_t> io_threads_native_;
};

std::unique_ptr<EchoServer> MakeServer(const BenchOptions& options)
{
    const auto& impl = options.impl;
    const auto& transport = options.transport;
#if !defined(BREEZE_BENCH_NO_TLS)
    if ((transport == "tls" || transport == "ws" || transport == "wss") && impl == "breeze-mainloop")
        return std::make_unique<MainLoopServer>(options, false, transport);
    if (transport == "tls")
        return std::make_unique<SecureServer>(/*tls=*/true, /*ws=*/false);
    if (transport == "ws")
        return std::make_unique<SecureServer>(/*tls=*/false, /*ws=*/true);
    if (transport == "wss")
        return std::make_unique<SecureServer>(/*tls=*/true, /*ws=*/true);
#endif
    (void)transport;
    if (impl.rfind("raw", 0) == 0 && impl != "raw-udp")
    {
        // raw | raw-iov | raw-iov+rdhup | raw-iov+cold | raw-iov+ptr | raw-iov+all（诊断变体，见 RawOptions）
        RawOptions o;
        o.iov = impl.find("-iov") != std::string::npos;
        o.rdhup = impl.find("+rdhup") != std::string::npos || impl.find("+all") != std::string::npos;
        o.cold = impl.find("+cold") != std::string::npos || impl.find("+all") != std::string::npos;
        o.ptr = impl.find("+ptr") != std::string::npos || impl.find("+all") != std::string::npos;
        return std::make_unique<RawServer>(o);
    }
    if (impl == "raw-udp")
        return std::make_unique<RawUdpServer>();
    if (impl == "rudp")
        return std::make_unique<RUdpBenchServer>(true, options);
    if (impl == "rudp-batched")
        return std::make_unique<RUdpBenchServer>(false, options);
    if (impl == "breeze")
        return std::make_unique<BreezeServer>(std::make_unique<BenchHandler>());
    if (impl == "breeze-copy")
        return std::make_unique<BreezeServer>(std::make_unique<CopyEchoHandler>());
    if (impl == "breeze-fixed")
        return std::make_unique<BreezeServer>(std::make_unique<FixedReplyHandler>(options.size));
    if (impl == "breeze-mainloop")
        return std::make_unique<MainLoopServer>(options, false);
    if (impl == "rudp-mainloop")
        return std::make_unique<MainLoopServer>(options, true);
#ifdef BREEZE_BENCH_MUDUO
    if (impl == "muduo")
        return std::make_unique<MuduoServer>();
#endif
    Die("未知的 --impl（或没有以 -DBREEZE_BUILD_BENCH=ON 编入 muduo）");
}

// ---- 压测客户端 ----

struct WorkerResult
{
    std::uint64_t msgs = 0;
    std::uint64_t replied_connections = 0;
    // 高连接饱和时RTT可能超过4.29秒，不能用32位纳秒截断尾延迟。
    std::vector<std::uint64_t> lat_ns;
};

class WorkerBase
{
public:
    virtual ~WorkerBase() = default;
    // 只让前 n 条连接 ping-pong（默认全部）；在 Run 之前调用。
    void SetActive(int n) { active_ = n; }
    // 每条连接两次发送的最小间隔；0 = 回包一到就发下一条（闭环全速）。在 Run 之前调用。
    void SetPeriod(std::chrono::nanoseconds p) { period_ = p; }
    virtual void Connect() = 0;
    virtual void Run(const std::atomic<bool>& measuring, const std::atomic<bool>& stop) = 0;
    virtual WorkerResult& result() = 0;

protected:
    int Active(int total) const { return active_ < 0 ? total : std::min(active_, total); }
    int active_ = -1;
    std::chrono::nanoseconds period_{0};
};

class Worker final : public WorkerBase
{
public:
    // first：本线程第一条连接在全部连接里的序号；scale：连接数很多，TCP 把连接分散到多个回环地址（见 kScaleThreshold）
    Worker(
        const Endpoint& ep, int conns, const std::vector<std::byte>& frame, int cpu, int first = 0, bool scale = false)
        : ep_(ep)
        , nconns_(conns)
        , frame_(frame)
        , cpu_(cpu)
        , first_(first)
        , scale_(scale)
    {
    }

    // 连好所有连接（不发数据）。
    void Connect() override
    {
        PinTo(cpu_);
        epfd_ = UniqueFd(::epoll_create1(EPOLL_CLOEXEC));
        conns_.resize(static_cast<std::size_t>(nconns_));
        for (int i = 0; i < nconns_; ++i)
        {
            Conn& c = conns_[static_cast<std::size_t>(i)];
            const Endpoint target = scale_ && ep_.kind() == Endpoint::Kind::Tcp ? ScaleTcpTarget(ep_, first_ + i) : ep_;
            REQUIRE(sock::CreateStreamSocket(target, c.fd) == 0);
            int rc;
            // UDS 的非阻塞 connect 在对方 accept 队列满时返回 EAGAIN（TCP 是 SYN
            // 重传，不会走到这里）：等服务端消化一会儿再试。
            while ((rc = sock::Connect(c.fd.get(), target)) == EAGAIN)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            REQUIRE(rc == 0 || rc == EINPROGRESS);
            pollfd p{c.fd.get(), POLLOUT, 0};
            const int ready = ::poll(&p, 1, 10000);
            const int wait_error = ready < 0 ? errno : 0;
            const int socket_error = sock::GetSocketError(c.fd.get());
            if (ready <= 0 || socket_error != 0)
                std::fprintf(stderr, "connect failed target=%s index=%d fd=%d poll=%d revents=%d errno=%d SO_ERROR=%d\n",
                             target.ToString().c_str(), first_ + i, c.fd.get(), ready,
                             static_cast<int>(p.revents), wait_error, socket_error);
            REQUIRE(ready > 0 && socket_error == 0);
            if (target.kind() == Endpoint::Kind::Tcp)
                (void)tcp::SetNoDelay(c.fd.get());
            c.rbuf.resize(frame_.size() * static_cast<std::size_t>(g_fanout));
            epoll_event ev{};
            ev.events = EPOLLIN;
            ev.data.u32 = static_cast<std::uint32_t>(i);
            REQUIRE(::epoll_ctl(epfd_.get(), EPOLL_CTL_ADD, c.fd.get(), &ev) == 0);
        }
    }

    // 开始压测，直到 stop。measuring 为 true 期间的完成数与延迟才计入结果。
    void Run(const std::atomic<bool>& measuring, const std::atomic<bool>& stop) override
    {
        PinTo(cpu_);
        const int active = Active(nconns_);
        const bool paced = period_.count() > 0;
        if (!paced)
        {
            for (int i = 0; i < active; ++i)
                SendNext(conns_[static_cast<std::size_t>(i)]);
        }
        else // 起点在一个周期内均匀错开，别让所有连接同相
        {
            const auto t0 = BenchClock::now();
            for (int i = 0; i < active; ++i)
            {
                Conn& c = conns_[static_cast<std::size_t>(i)];
                c.due = t0 + period_ * i / std::max(active, 1);
                starts_.push_back({c.due, static_cast<std::uint32_t>(i)});
            }
        }
        ::prctl(PR_SET_TIMERSLACK, 1UL, 0UL, 0UL, 0UL);
        std::vector<epoll_event> evs(1024);
        bool verified_all = false;
        while (!stop.load(std::memory_order_relaxed))
        {
            auto timeout = std::chrono::nanoseconds(std::chrono::milliseconds(50));
            if (!starts_.empty())
                timeout = std::min(
                    timeout,
                    std::chrono::duration_cast<std::chrono::nanoseconds>(starts_.front().first - BenchClock::now()));
            const int n = EpollWaitFor(epfd_.get(), evs.data(), static_cast<int>(evs.size()), timeout);
            for (int i = 0; i < n; ++i)
            {
                const std::uint32_t idx = evs[static_cast<std::size_t>(i)].data.u32;
                Conn& c = conns_[idx];
                const std::size_t reply_bytes = frame_.size() * static_cast<std::size_t>(g_fanout);
                const ssize_t r = ::read(c.fd.get(), c.rbuf.data() + c.got, reply_bytes - c.got);
                if (r <= 0)
                {
                    if (r < 0 && (errno == EAGAIN || errno == EINTR))
                        continue;
                    Die("压测中连接被对端关闭或出错");
                }
                c.got += static_cast<std::size_t>(r);
                if (c.got < reply_bytes)
                    continue;
                // 每条连接的第一个回包逐字节核对：别拿一个回显错了的实现去比性能。
                if (!c.verified)
                {
                    for (int k = 0; k < g_fanout; ++k)
                        REQUIRE(std::memcmp(c.rbuf.data() + static_cast<std::size_t>(k) * frame_.size(),
                                            frame_.data(),
                                            frame_.size()) == 0);
                    c.verified = true;
                    ++result_.replied_connections;
                }
                if (measuring.load(std::memory_order_relaxed))
                {
                    const auto lat =
                        std::chrono::duration_cast<std::chrono::nanoseconds>(BenchClock::now() - c.sent_at).count();
                    result_.lat_ns.push_back(static_cast<std::uint64_t>(std::max<std::int64_t>(lat, 0)));
                    ++result_.msgs;
                }
                c.got = 0;
                if (paced)
                {
                    c.due += period_; // 按到期时刻累加，不从实际发送时刻算：晚了的发送不让速率往下漂
                    starts_.push_back({c.due, idx});
                }
                else
                    SendNext(c);
            }
            if (!starts_.empty())
            {
                const auto now = BenchClock::now();
                while (!starts_.empty() && starts_.front().first <= now)
                {
                    const std::uint32_t idx = starts_.front().second;
                    starts_.pop_front();
                    SendNext(conns_[idx]);
                }
            }
            (void)verified_all;
        }
    }

    WorkerResult& result() override { return result_; }

private:
    struct Conn
    {
        UniqueFd fd;
        std::vector<std::byte> rbuf;
        std::size_t got = 0;
        bool verified = false;
        BenchClock::time_point sent_at;
        BenchClock::time_point due; // 定速时：下一次该发送的时刻
    };

    void SendNext(Conn& c)
    {
        c.sent_at = BenchClock::now();
        std::size_t off = 0;
        while (off < frame_.size())
        {
            const ssize_t w = ::send(c.fd.get(), frame_.data() + off, frame_.size() - off, MSG_NOSIGNAL);
            if (w > 0)
                off += static_cast<std::size_t>(w);
            else if (w < 0 && errno == EAGAIN)
            {
                pollfd p{c.fd.get(), POLLOUT, 0};
                ::poll(&p, 1, 100);
            }
            else
                Die("压测客户端写失败");
        }
    }

    Endpoint ep_;
    int nconns_;
    const std::vector<std::byte>& frame_;
    int cpu_;
    int first_;
    bool scale_;
    UniqueFd epfd_;
    std::vector<Conn> conns_;
    std::deque<std::pair<BenchClock::time_point, std::uint32_t>> starts_; // 定速时：各连接下一次发送的时刻
    WorkerResult result_;
};

#if !defined(BREEZE_BENCH_NO_TLS)
// tls / ws / wss 的压测客户端：每条连接一个本库 StreamClient（同一个 EventLoopGroup 的 1 个 IO 线程），
// 回包在 IO 线程回调里直接发下一条（闭环），延迟从发出到收到完整消息。不用 raw 客户端：TLS / WebSocket 必须由库来分帧。
class SecureWorker final : public WorkerBase
{
public:
    SecureWorker(const Endpoint& ep, int conns, std::vector<std::byte> payload, int cpu, bool tls, bool ws)
        : ep_(ep)
        , nconns_(conns)
        , payload_(std::move(payload))
        , cpu_(cpu)
        , tls_(tls)
        , ws_(ws)
    {
    }

    void Connect() override
    {
        PinTo(cpu_);
        if (ws_)
        {
            WsClientOptions wo;
            wo.message_type = WsMessageType::Binary;
            wo.max_message_bytes = 1 << 20;
            ws_factory_ = std::make_unique<WsClientCodecFactory>(std::move(wo));
        }
        else
        {
            lp_factory_ = std::make_unique<LengthPrefixCodecFactory>(1024 * 1024);
        }
        if (tls_)
        {
            std::string err;
            tls_ctx_ = TlsContext::CreateClient(BenchClientTls(), err);
            REQUIRE(tls_ctx_ != nullptr);
        }
        group_ = std::make_unique<EventLoopGroup>(EventLoopGroupOptions{1, g_table_slots});
        REQUIRE(group_->Start() == 0);
        const IFrameCodecFactory& factory = ws_ ? static_cast<const IFrameCodecFactory&>(*ws_factory_) : *lp_factory_;
        clients_.reserve(static_cast<std::size_t>(nconns_));
        handlers_.reserve(static_cast<std::size_t>(nconns_));
        handles_.assign(static_cast<std::size_t>(nconns_), ConnectionHandle{});
        conns_state_.resize(static_cast<std::size_t>(nconns_));
        for (int i = 0; i < nconns_; ++i)
        {
            handlers_.push_back(std::make_unique<Handler>(*this, i));
            StreamClientOptions co;
            co.connection.high_water_mark = 64u << 20;
            co.tls = tls_ctx_;
            clients_.push_back(std::make_unique<StreamClient>(
                *group_, ep_, std::move(co), ReconnectPolicy{20ms, 500ms, 2.0, 0.0}, *handlers_.back(), factory));
            REQUIRE(clients_.back()->Start() == 0);
        }
    }

    void Run(const std::atomic<bool>& measuring, const std::atomic<bool>& stop) override
    {
        PinTo(cpu_);
        measuring_ = &measuring;
        const auto deadline = BenchClock::now() + std::chrono::seconds(30);
        while (connected_.load(std::memory_order_acquire) < nconns_ && BenchClock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (connected_.load(std::memory_order_acquire) < nconns_)
            Die("TLS / WebSocket 客户端没能在 30 秒内全部连接成功（握手失败？看服务端日志）");
        loop_ = &GroupAccess::LoopAt(*group_, 0);
        const int active = Active(nconns_);
        paced_ = period_.count() > 0;
        const bool paced = paced_;
        loop_->Post(
            [this, active, paced]
            {
                for (int i = 0; i < active; ++i)
                {
                    if (!paced)
                        SendNext(i);
                    else // 起点在一个周期内均匀错开；之后按到期时刻累加（与 Worker 一样，不让 RTT 拖慢速率）
                    {
                        conns_state_[static_cast<std::size_t>(i)].due =
                            BenchClock::now() + std::chrono::nanoseconds(period_.count() * i / std::max(active, 1));
                        ScheduleDue(i);
                    }
                }
            });
        while (!stop.load(std::memory_order_relaxed))
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        stopping_ = true; // 停机强关的连接会回调 OnClosed：那是我们自己关的，不算异常
        group_->Shutdown(std::chrono::milliseconds::zero());
        measuring_ = nullptr;
        if (closed_.load() > 0)
            Die("压测中连接被对端关闭或出错");
    }

    WorkerResult& result() override { return result_; }

    // 以下由 IO 线程的 Handler 调用。
    void OnConnected(int i, ConnectionHandle h)
    {
        handles_[static_cast<std::size_t>(i)] = h;
        connected_.fetch_add(1, std::memory_order_release);
    }
    void OnReply(int i, std::span<const std::byte> msg)
    {
        auto& c = conns_state_[static_cast<std::size_t>(i)];
        if (!c.verified)
        {
            REQUIRE(msg.size() == payload_.size() &&
                    std::memcmp(msg.data(), payload_.data(), payload_.size()) == 0); // 别拿回显错了的实现比性能
        }
        if (++c.got < g_fanout) // 服务端对每条请求回 g_fanout 条：等齐才算一次往返
            return;
        c.got = 0;
        if (!c.verified)
            ++result_.replied_connections;
        c.verified = true;
        if (measuring_ != nullptr && measuring_->load(std::memory_order_relaxed))
        {
            const auto lat =
                std::chrono::duration_cast<std::chrono::nanoseconds>(BenchClock::now() - c.sent_at).count();
            result_.lat_ns.push_back(static_cast<std::uint64_t>(std::max<std::int64_t>(lat, 0)));
            ++result_.msgs;
        }
        if (!paced_)
            SendNext(i); // 定速时由定时器驱动，回包不触发下一条
    }
    void OnClosed(int)
    {
        if (!stopping_)
            closed_.fetch_add(1);
    }

private:
    class Handler final : public IConnectionHandler
    {
    public:
        Handler(SecureWorker& w, int i)
            : w_(w)
            , i_(i)
        {
        }
        void OnConnected(IConnection& c) noexcept override { w_.OnConnected(i_, c.handle()); }
        void OnMessage(IConnection&, std::span<const std::byte> msg) noexcept override { w_.OnReply(i_, msg); }
        void OnClosed(IConnection&, CloseReason, int) noexcept override { w_.OnClosed(i_); }

    private:
        SecureWorker& w_;
        int i_;
    };

    struct ConnState
    {
        bool verified = false;
        int got = 0; // 本次往返已收到的回复条数
        BenchClock::time_point sent_at;
        BenchClock::time_point due; // 定速：下一次该发送的时刻
    };

    void SendNext(int i)
    {
        const ConnectionHandle h = handles_[static_cast<std::size_t>(i)];
        if (!h.valid())
            return;
        auto& st = conns_state_[static_cast<std::size_t>(i)];
        st.sent_at = BenchClock::now();
        (void)h.Send(SharedBuffer::CopyFrom(payload_));
        if (paced_)
        {
            st.due += period_;
            ScheduleDue(i);
        }
    }

    void ScheduleDue(int i)
    {
        auto delay = conns_state_[static_cast<std::size_t>(i)].due - BenchClock::now();
        if (delay < std::chrono::nanoseconds::zero())
            delay = std::chrono::nanoseconds::zero();
        loop_->RunAfter(std::chrono::duration_cast<std::chrono::steady_clock::duration>(delay),
                        [this, i] { SendNext(i); });
    }

    Endpoint ep_;
    int nconns_;
    std::vector<std::byte> payload_;
    int cpu_;
    bool tls_;
    bool ws_;
    std::shared_ptr<const IStreamLayerFactory> tls_ctx_;
    std::unique_ptr<LengthPrefixCodecFactory> lp_factory_;
    std::unique_ptr<WsClientCodecFactory> ws_factory_;
    std::unique_ptr<EventLoopGroup> group_;
    std::vector<std::unique_ptr<Handler>> handlers_;
    std::vector<std::unique_ptr<StreamClient>> clients_; // 在 group_ 之后声明：先销毁
    std::vector<ConnectionHandle> handles_;
    std::vector<ConnState> conns_state_;
    EventLoop* loop_ = nullptr;
    bool paced_ = false;
    const std::atomic<bool>* measuring_ = nullptr;
    std::atomic<int> connected_{0};
    std::atomic<int> closed_{0};
    std::atomic<bool> stopping_{false};
    WorkerResult result_;
};

#endif // !BREEZE_BENCH_NO_TLS

// UDP 压测客户端：每条连接一个 connect 过的 UDP socket，闭环 ping-pong。
// kcp = true 时先凭票据握手、之后每条消息走 KcpSession。
// 丢包兜底：回环上只有服务端接收缓冲溢出才会丢（启动时一瞬间 N 条连接同时发第一条）。
class UdpWorker final : public WorkerBase
{
public:
    UdpWorker(const Endpoint& ep,
              int conns,
              const std::vector<std::byte>& payload,
              int cpu,
              bool kcp,
              std::vector<RUdpTicket> tickets,
              int delay_ms = 0)
        : ep_(ep)
        , nconns_(conns)
        , payload_(payload)
        , cpu_(cpu)
        , kcp_(kcp)
        , tickets_(std::move(tickets))
        , delay_(std::chrono::milliseconds(delay_ms))
        , retry_after_(std::chrono::milliseconds(100 + 8 * delay_ms)) // 有延迟时别把正常的往返当成丢包
    {
        if (kcp_)
            REQUIRE(tickets_.size() >= static_cast<std::size_t>(conns));
    }

    void Connect() override
    {
        PinTo(cpu_);
        epfd_ = UniqueFd(::epoll_create1(EPOLL_CLOEXEC));
        conns_.resize(static_cast<std::size_t>(nconns_)); // 之后不再增长：KcpSession 的 output 里存着 Conn 的地址
        for (int i = 0; i < nconns_; ++i)
        {
            Conn& c = conns_[static_cast<std::size_t>(i)];
            c.owner = this;
            c.index = static_cast<std::uint32_t>(i);
            REQUIRE(sock::CreateSocket(ep_, SOCK_DGRAM, c.fd) == 0);
            const int buf = 1 << 20;
            ::setsockopt(c.fd.get(), SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));
            REQUIRE(sock::Connect(c.fd.get(), ep_) == 0); // connect 时内核自动绑定源端口
            if (kcp_)
                Handshake(c, tickets_[static_cast<std::size_t>(i)]);
            epoll_event ev{};
            ev.events = EPOLLIN;
            ev.data.u32 = static_cast<std::uint32_t>(i);
            REQUIRE(::epoll_ctl(epfd_.get(), EPOLL_CTL_ADD, c.fd.get(), &ev) == 0);
        }
    }

    void Run(const std::atomic<bool>& measuring, const std::atomic<bool>& stop) override
    {
        PinTo(cpu_);
        const int active = Active(nconns_);
        const bool delayed = delay_.count() > 0;
        std::vector<epoll_event> evs(1024);
        auto last_scan = BenchClock::now();
        const bool paced = period_.count() > 0;
        if (!delayed && !paced)
        {
            for (int i = 0; i < active; ++i)
                SendNext(conns_[static_cast<std::size_t>(i)]);
        }
        else
        {
            // 有延迟（或定速）时把各连接的起点在一个往返（或一个周期）内均匀错开：闭环 + 固定延迟会让同时起步的连接
            // 一直同相，每个往返都冲来一波突发，把服务端的接收缓冲打爆（丢包），污染要比较的 CPU。
            const auto span = paced ? period_ : 2 * delay_;
            for (int i = 0; i < active; ++i)
            {
                Conn& c = conns_[static_cast<std::size_t>(i)];
                c.due = last_scan + span * i / std::max(active, 1);
                starts_.push_back({c.due, static_cast<std::uint32_t>(i)});
            }
        }
        ::prctl(PR_SET_TIMERSLACK, 1UL, 0UL, 0UL, 0UL);
        while (!stop.load(std::memory_order_relaxed))
        {
            auto timeout = std::chrono::nanoseconds(std::chrono::milliseconds(10));
            if (delayed || !starts_.empty()) // 睡到最近一个延迟 / 定速项到期，最多 10ms
            {
                const auto now0 = BenchClock::now();
                auto next = now0 + timeout;
                if (!rx_.empty())
                    next = std::min(next, rx_.front().due);
                if (!tx_.empty())
                    next = std::min(next, tx_.front().due);
                if (!starts_.empty())
                    next = std::min(next, starts_.front().first);
                timeout = std::chrono::duration_cast<std::chrono::nanoseconds>(next - now0);
            }
            const int n = EpollWaitFor(epfd_.get(), evs.data(), static_cast<int>(evs.size()), timeout);
            for (int i = 0; i < n; ++i)
                OnReadable(conns_[evs[static_cast<std::size_t>(i)].data.u32], measuring);
            const auto now = BenchClock::now();
            while (!starts_.empty() && starts_.front().first <= now)
            {
                const std::uint32_t i = starts_.front().second;
                starts_.pop_front();
                SendNext(conns_[i]);
            }
            if (delayed)
            {
                // 到期的收包先处理（处理里会产生新的发包，到期时刻在 now 之后，不会在这一轮里被发出）
                while (!rx_.empty() && rx_.front().due <= now)
                {
                    Delayed d = std::move(rx_.front());
                    rx_.pop_front();
                    Process(conns_[d.conn], d.data, measuring);
                }
                while (!tx_.empty() && tx_.front().due <= now)
                {
                    Delayed d = std::move(tx_.front());
                    tx_.pop_front();
                    (void)::send(conns_[d.conn].fd.get(), d.data.data(), d.data.size(), MSG_NOSIGNAL);
                }
            }
            if (now - last_scan > std::chrono::milliseconds(20))
            {
                last_scan = now;
                for (int i = 0; i < active; ++i)
                {
                    Conn& c = conns_[static_cast<std::size_t>(i)];
                    if (c.pending && now - c.last_tx > retry_after_) // 发出去很久没回包：丢了
                        Retry(c);
                }
            }
        }
    }

    WorkerResult& result() override { return result_; }

private:
    struct Conn
    {
        UniqueFd fd;
        std::unique_ptr<KcpSession> kcp;
        UdpWorker* owner = nullptr;
        std::uint32_t index = 0;
        bool verified = false;
        bool pending = false;       // 有一条已发出、还没收到回包的请求（定速时连接大部分时间在等下一次发送，不是丢包）
        BenchClock::time_point due; // 定速时：下一次该发送的时刻
        BenchClock::time_point sent_at; // 这条消息第一次发出的时刻：延迟从这里算，丢包的等待也算进去
        BenchClock::time_point last_tx; // 最近一次（含重发）发出的时刻：决定什么时候重发
    };

    // 一个要延迟发出 / 延迟处理的数据报；延迟固定，所以队列天然按到期时间排好序。
    struct Delayed
    {
        BenchClock::time_point due;
        std::uint32_t conn;
        std::vector<std::byte> data;
    };

    // 发一个数据报：有延迟就排队，到期再发；没有就直接发。
    void Transmit(Conn& c, std::span<const std::byte> data)
    {
        if (delay_.count() > 0)
            tx_.push_back({BenchClock::now() + delay_, c.index, {data.begin(), data.end()}});
        else
            (void)::send(c.fd.get(), data.data(), data.size(), MSG_NOSIGNAL);
    }

    static void Output(void* user, std::span<const std::byte> seg)
    {
        auto* c = static_cast<Conn*>(user);
        const std::byte hdr = rudp::DataHeader();
        if (c->owner->delay_.count() > 0)
        {
            std::vector<std::byte> d;
            d.reserve(seg.size() + 1);
            d.push_back(hdr);
            d.insert(d.end(), seg.begin(), seg.end());
            c->owner->Transmit(*c, d);
            return;
        }
        iovec iov[2] = {{const_cast<std::byte*>(&hdr), 1}, {const_cast<std::byte*>(seg.data()), seg.size()}};
        msghdr mh{};
        mh.msg_iov = iov;
        mh.msg_iovlen = 2;
        (void)::sendmsg(c->fd.get(), &mh, MSG_NOSIGNAL);
    }

    void Handshake(Conn& c, const RUdpTicket& t)
    {
        bool ok = false;
        for (int attempt = 0; attempt < 20 && !ok; ++attempt)
        {
            const rudp::Control hello = rudp::EncodeHello(t.conv, t.token);
            (void)::send(c.fd.get(), hello.bytes.data(), hello.size, MSG_NOSIGNAL);
            pollfd p{c.fd.get(), POLLIN, 0};
            if (::poll(&p, 1, 500) <= 0)
                continue;
            std::byte buf[64];
            const ssize_t n = ::recv(c.fd.get(), buf, sizeof(buf), MSG_DONTWAIT);
            if (n <= 0)
                continue;
            const auto pk = rudp::Parse({buf, static_cast<std::size_t>(n)});
            ok = pk && pk->type == rudp::PacketType::Accept && pk->conv == t.conv;
        }
        REQUIRE(ok);
        c.kcp = KcpSession::Create(t.conv, RUdpOptions{}, &UdpWorker::Output, &c);
        REQUIRE(c.kcp != nullptr);
        c.kcp->Update(KcpNowMs());
    }

    void SendNext(Conn& c)
    {
        c.sent_at = c.last_tx = BenchClock::now();
        c.pending = true;
        if (!kcp_)
        {
            Transmit(c, payload_);
            return;
        }
        // 与库里 flush_on_send 的客户端同一个写法：Send 加 Flush(now)，待发的确认和数据装进同一个数据报。
        // 以前这里先 Update：往返时间超过 KCP 的 10ms 门限时 Update 每次都会 flush，把确认单独发一个数据报，
        // 每条消息变成两个数据报，服务端的包率翻倍（回环上两次发送间隔不到 10ms，过不了门限，所以看不出来）。
        std::uint32_t frags = 0;
        REQUIRE(c.kcp->Send(payload_, frags) == 0);
        c.kcp->Flush(KcpNowMs());
    }

    void Retry(Conn& c)
    {
        c.last_tx = BenchClock::now(); // sent_at 不动：延迟里包含这次等待，丢包的代价算在数字上
        if (!kcp_)
            Transmit(c, payload_);
        else
            c.kcp->Update(KcpNowMs()); // 驱动 KCP 的超时重传
    }

    void Complete(Conn& c, std::span<const std::byte> reply, const std::atomic<bool>& measuring)
    {
        if (reply.size() != payload_.size())
            return; // 不是这条连接的当前回包（重发造成的重复回显）
        if (!c.verified)
        {
            REQUIRE(std::memcmp(reply.data(), payload_.data(), payload_.size()) == 0);
            c.verified = true;
            ++result_.replied_connections;
        }
        if (measuring.load(std::memory_order_relaxed))
        {
            const auto lat =
                std::chrono::duration_cast<std::chrono::nanoseconds>(BenchClock::now() - c.sent_at).count();
            result_.lat_ns.push_back(static_cast<std::uint64_t>(std::max<std::int64_t>(lat, 0)));
            ++result_.msgs;
        }
        c.pending = false;
        if (period_.count() == 0)
        {
            SendNext(c);
            return;
        }
        // 定速：真实的客户端收到数据后很快（至多一个 interval）就会把确认发出去，不会等到下一条请求才顺带；
        // 稀疏的消息下这就是单独的一个数据报，服务端要多收一个。
        if (kcp_)
            c.kcp->Flush(KcpNowMs());
        c.due += period_; // 按到期时刻累加，不从实际发送时刻算：晚了的发送不让速率往下漂
        starts_.push_back({c.due, c.index});
    }

    void OnReadable(Conn& c, const std::atomic<bool>& measuring)
    {
        static thread_local std::byte buf[2048];
        for (;;)
        {
            const ssize_t n = ::recv(c.fd.get(), buf, sizeof(buf), MSG_DONTWAIT);
            if (n < 0)
            {
                if (errno == EAGAIN || errno == EINTR)
                    return;
                Die("压测中 UDP 连接出错");
            }
            const std::span<const std::byte> data{buf, static_cast<std::size_t>(n)};
            if (delay_.count() > 0) // 排队，到期再处理
                rx_.push_back({BenchClock::now() + delay_, c.index, {data.begin(), data.end()}});
            else
                Process(c, data, measuring);
        }
    }

    // 处理一个收到的数据报。
    void Process(Conn& c, std::span<const std::byte> data, const std::atomic<bool>& measuring)
    {
        static thread_local std::vector<std::byte> msg(64 * 1024);
        if (!kcp_)
        {
            Complete(c, data, measuring);
            return;
        }
        const auto pk = rudp::Parse(data);
        if (!pk || pk->type != rudp::PacketType::Data)
            return;
        if (c.kcp->Input(pk->body, KcpNowMs()) < 0)
            return;
        for (int sz; (sz = c.kcp->PeekSize()) >= 0;)
        {
            REQUIRE(static_cast<std::size_t>(sz) <= msg.size());
            REQUIRE(c.kcp->Recv({msg.data(), static_cast<std::size_t>(sz)}) == sz);
            Complete(c, {msg.data(), static_cast<std::size_t>(sz)}, measuring);
        }
    }

    Endpoint ep_;
    int nconns_;
    const std::vector<std::byte>& payload_;
    int cpu_;
    bool kcp_;
    std::vector<RUdpTicket> tickets_;
    std::chrono::milliseconds delay_;
    std::chrono::milliseconds retry_after_;
    std::deque<Delayed> rx_, tx_;
    std::deque<std::pair<BenchClock::time_point, std::uint32_t>>
        starts_; // 各连接下一次发送的时刻（有延迟或定速时才用）
    UniqueFd epfd_;
    std::vector<Conn> conns_;
    WorkerResult result_;
};

// /proc/net/snmp 里 UDP 的某个计数（全系统累计，只有压测在跑，可以当作本进程的）；读不到返回 0。
// RcvbufErrors：接收缓冲满而丢的数据报；InDatagrams / OutDatagrams：交给 / 发出的数据报数（含压测客户端的 socket）。
std::uint64_t UdpSnmp(const char* field)
{
    FILE* f = std::fopen("/proc/net/snmp", "r");
    if (f == nullptr)
        return 0;
    char line[1024];
    std::vector<std::string> names;
    std::uint64_t result = 0;
    while (std::fgets(line, sizeof(line), f) != nullptr)
    {
        if (std::strncmp(line, "Udp:", 4) != 0)
            continue;
        std::vector<std::string> toks;
        for (char* tok = std::strtok(line + 4, " \n"); tok != nullptr; tok = std::strtok(nullptr, " \n"))
            toks.emplace_back(tok);
        if (names.empty())
        {
            names = toks; // 第一行是列名，第二行是值
            continue;
        }
        for (std::size_t i = 0; i < names.size() && i < toks.size(); ++i)
            if (names[i] == field)
                result = std::strtoull(toks[i].c_str(), nullptr, 10);
        break;
    }
    std::fclose(f);
    return result;
}

std::uint64_t UdpRcvbufErrors() { return UdpSnmp("RcvbufErrors"); }

// /proc/net/udp 里本地端口为 port 的 socket 的 drops（这个 socket 因接收缓冲满而丢的数据报数）。
// 与 UdpRcvbufErrors 的区别：后者是全系统（含压测客户端的 10000 个 socket），这个只看服务端自己的。找不到返回 0。
std::uint64_t UdpSocketDrops(std::uint16_t port)
{
    FILE* f = std::fopen("/proc/net/udp", "r");
    if (f == nullptr)
        return 0;
    char line[512];
    char want[16];
    std::snprintf(want, sizeof(want), ":%04X ", port);
    std::uint64_t total = 0;
    while (std::fgets(line, sizeof(line), f) != nullptr)
    {
        // 本地地址是第二列（偏移约 8–14）；对端地址在后面（偏移 > 24），客户端 socket 的对端端口就是 port，要排除。
        const char* local = std::strstr(line, want);
        if (local == nullptr || local - line > 24)
            continue;
        // 行尾是 "... ref pointer drops"：取最后一列
        std::string row(line);
        while (!row.empty() && (row.back() == '\n' || row.back() == ' '))
            row.pop_back();
        const auto sp = row.rfind(' ');
        if (sp != std::string::npos)
            total += std::strtoull(row.c_str() + sp + 1, nullptr, 10);
    }
    std::fclose(f);
    return total;
}

// 环境变量 BENCH_VERBOSE 非空时，在 stderr 打各阶段的起止时刻（排查"为什么这一组跑得特别久"用）。
void Phase(const char* what)
{
    static const bool on = std::getenv("BENCH_VERBOSE") != nullptr;
    static const auto t0 = BenchClock::now();
    if (on)
        std::fprintf(stderr, "[%7.2fs] %s\n", std::chrono::duration<double>(BenchClock::now() - t0).count(), what);
}

double ThreadCpuSeconds(pthread_t t)
{
    clockid_t cid;
    timespec ts{};
    if (pthread_getcpuclockid(t, &cid) != 0 || clock_gettime(cid, &ts) != 0)
        return 0;
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

double ServerCpuSeconds(EchoServer& server)
{
    double total = 0;
    for (const pthread_t thread : server.ServerThreads())
        total += ThreadCpuSeconds(thread);
    return total;
}

double Percentile(const std::vector<std::uint64_t>& sorted, double p)
{
    if (sorted.empty())
        return 0;
    const std::size_t idx =
        std::min(sorted.size() - 1, static_cast<std::size_t>(p * static_cast<double>(sorted.size())));
    return static_cast<double>(sorted[idx]);
}

// 当前进程的 RSS（KB）。
long RssKb()
{
    FILE* f = std::fopen("/proc/self/status", "r");
    REQUIRE(f != nullptr);
    char line[256];
    long kb = 0;
    while (std::fgets(line, sizeof(line), f) != nullptr)
        if (std::sscanf(line, "VmRSS: %ld kB", &kb) == 1)
            break;
    std::fclose(f);
    return kb;
}

// 内存探测：服务端起来之后记一次 RSS，建 N 条空闲连接（只握手 / 连上，不发数据），等服务端都登记完再记一次，
// 差值 / N 就是每条连接在服务端进程里占的内存。客户端侧只有 fd（内核对象不计入用户 RSS）。
// rudp 的会话含 KcpSession：ikcp_setmtu 按 (mtu + 24) × 3 分配一块收发缓冲，这是大头。
int MemProbe(const BenchOptions& o, const Endpoint& ep, EchoServer& server, bool kcp)
{
    std::vector<RUdpTicket> tickets;
    if (kcp)
        tickets = server.IssueTickets(o.mem_probe);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const long before = RssKb();
    std::vector<UniqueFd> fds;
    for (int i = 0; i < o.mem_probe; ++i)
    {
        UniqueFd fd;
        if (o.transport == "udp")
        {
            REQUIRE(sock::CreateSocket(ep, SOCK_DGRAM, fd) == 0);
            REQUIRE(sock::Connect(fd.get(), ep) == 0);
            if (kcp)
            {
                const RUdpTicket& t = tickets[static_cast<std::size_t>(i)];
                bool ok = false;
                for (int attempt = 0; attempt < 20 && !ok; ++attempt)
                {
                    const rudp::Control hello = rudp::EncodeHello(t.conv, t.token);
                    (void)::send(fd.get(), hello.bytes.data(), hello.size, MSG_NOSIGNAL);
                    pollfd p{fd.get(), POLLIN, 0};
                    if (::poll(&p, 1, 500) <= 0)
                        continue;
                    std::byte buf[64];
                    const ssize_t n = ::recv(fd.get(), buf, sizeof(buf), MSG_DONTWAIT);
                    const auto pk = n > 0 ? rudp::Parse({buf, static_cast<std::size_t>(n)}) : std::nullopt;
                    ok = pk && pk->type == rudp::PacketType::Accept;
                }
                REQUIRE(ok);
            }
        }
        else
        {
            const Endpoint target = ep.kind() != Endpoint::Kind::Tcp ? ep
                                    : o.mem_probe > kScaleThreshold  ? ScaleTcpTarget(ep, i)
                                                                     : *Endpoint::Tcp("127.0.0.1", ep.port());
            REQUIRE(sock::CreateStreamSocket(target, fd) == 0);
            int rc;
            while ((rc = sock::Connect(fd.get(), target)) == EAGAIN)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            REQUIRE(rc == 0 || rc == EINPROGRESS);
            pollfd p{fd.get(), POLLOUT, 0};
            REQUIRE(::poll(&p, 1, 10000) > 0 && sock::GetSocketError(fd.get()) == 0);
        }
        fds.push_back(std::move(fd));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1000)); // 服务端 accept / 登记完
    const long after = RssKb();
    // 这些连接空闲着：服务端 IO 线程这 3 秒里还要用多少 CPU（rudp 每 10ms 一拍的定时器要走遍所有会话）
    const double cpu0 = ServerCpuSeconds(server);
    std::this_thread::sleep_for(std::chrono::seconds(3));
    const double idle_cpu = (ServerCpuSeconds(server) - cpu0) / 3.0 * 100.0;
    std::printf(
        "MEM impl=%s transport=%s conns=%d rss_before_kb=%ld rss_after_kb=%ld bytes_per_conn=%.0f "
        "idle_cpu_pct=%.2f reuse_port=%s io_threads=%zu\n",
        o.impl.c_str(),
        o.transport.c_str(),
        o.mem_probe,
        before,
        after,
        static_cast<double>(after - before) * 1024.0 / o.mem_probe,
        idle_cpu,
        o.reuse_port ? "true" : "false",
        server.ServerThreads().size());
    fds.clear();
    server.Stop();
    if (o.transport == "uds")
        ::unlink(ep.address().c_str());
    return 0;
}

// ---- 重连风暴（--storm N）----
//
// 量"跨 loop accept"这条路径占的比例：服务端是 production 的 StreamServer（g_io_threads 个 loop，accept 在 loop 0，
// 连接按"最少的 loop"分给各 loop，非本 loop 的经 EventLoop::Post 投递一个带 UniqueFd + PeerInfo 的闭包）；
// N 个客户端线程不停 connect → 立刻以 RST 断开（SO_LINGER 0，不留 TIME_WAIT，本机临时端口只有 4096 个）。
// 输出每次 accept 服务端所有 IO 线程合计的 CPU 微秒，以及同一个闭包（同样的捕获、同样的 move_only_function 与
// make_unique）单独的 构造 + 销毁 成本——后者除以前者就是这条投递路径里"分配"占 accept 的比例。
double ClosureAllocNs()
{
    struct Captures
    {
        void* self;
        void* target;
        UniqueFd fd;
        PeerInfo info;
    };
    constexpr int kIters = 2'000'000;
    const auto t0 = BenchClock::now();
    std::uint64_t sink = 0;
    for (int i = 0; i < kIters; ++i)
    {
        Captures c{&sink, &sink, UniqueFd{}, PeerInfo{}};
        // 与 EventLoop::Post 相同：lambda → move_only_function → make_unique<move_only_function>
        auto task = std::make_unique<std::move_only_function<void()>>(std::move_only_function<void()>(
            [c = std::move(c), &sink]() mutable { sink += reinterpret_cast<std::uintptr_t>(c.self); }));
        asm volatile("" ::"r"(task.get()) : "memory");
    }
    const double ns = std::chrono::duration<double, std::nano>(BenchClock::now() - t0).count() / kIters;
    if (sink == 42)
        std::puts("");
    return ns;
}

int Storm(const BenchOptions& o)
{
    class NullHandler final : public IConnectionHandler
    {
    public:
        void OnMessage(IConnection&, std::span<const std::byte>) noexcept override {}
    } handler;
    LengthPrefixCodecFactory factory(1 << 20);
    EventLoopGroup group({static_cast<std::uint32_t>(g_io_threads), 65536});
    StreamServerOptions so;
    so.backlog = 4096;
    StreamServer server(group, *Endpoint::Tcp("127.0.0.1", 0), std::move(so), handler, factory);
    REQUIRE(group.Start() == 0);
    REQUIRE(server.Start() == 0);
    const std::uint16_t port = server.endpoint().port();

    std::vector<pthread_t> io_threads;
    for (int i = 0; i < g_io_threads; ++i)
    {
        std::promise<pthread_t> ready;
        GroupAccess::LoopAt(group, static_cast<std::size_t>(i)).Post([&ready] { ready.set_value(::pthread_self()); });
        io_threads.push_back(ready.get_future().get());
    }

    std::atomic<bool> stop{false};
    std::vector<std::thread> clients;
    for (int t = 0; t < g_storm_threads; ++t)
    {
        clients.emplace_back(
            [&, t]
            {
                if (!o.client_cpus.empty())
                    PinTo(o.client_cpus[static_cast<std::size_t>(t) % o.client_cpus.size()]);
                const Endpoint ep = *Endpoint::Tcp("127.0.0.1", port);
                while (!stop.load(std::memory_order_relaxed))
                {
                    UniqueFd fd;
                    if (sock::CreateStreamSocket(ep, fd) != 0)
                        continue;
                    const linger lg{1, 0}; // close 发 RST：不进 TIME_WAIT
                    ::setsockopt(fd.get(), SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
                    int rc;
                    while ((rc = sock::Connect(fd.get(), ep)) == EAGAIN)
                        std::this_thread::yield();
                    if (rc == EINPROGRESS)
                    {
                        pollfd p{fd.get(), POLLOUT, 0};
                        if (::poll(&p, 1, 1000) <= 0)
                            continue;
                    }
                } // fd 析构 = close
            });
    }

    const auto cpu_total = [&]
    {
        double sum = 0;
        for (const pthread_t th : io_threads)
            sum += ThreadCpuSeconds(th);
        return sum;
    };
    std::this_thread::sleep_for(std::chrono::seconds(o.warmup_s));
    const std::uint64_t acc0 = group.GetStats().accepted;
    const double cpu0 = cpu_total();
    const auto t0 = BenchClock::now();
    std::this_thread::sleep_for(std::chrono::seconds(o.duration_s));
    const std::uint64_t acc1 = group.GetStats().accepted;
    const double cpu1 = cpu_total();
    const double secs = std::chrono::duration<double>(BenchClock::now() - t0).count();
    stop = true;
    for (auto& c : clients)
        c.join();
    group.Shutdown(std::chrono::milliseconds(0));

    const double accepted = static_cast<double>(acc1 - acc0);
    const double us_per_accept = accepted > 0 ? (cpu1 - cpu0) * 1e6 / accepted : 0.0;
    const double alloc_ns = ClosureAllocNs();
    std::printf(
        "STORM io_threads=%d client_threads=%d accepted_per_s=%.0f server_cpu_us_per_accept=%.2f closure_alloc_ns=%.0f "
        "closure_share_pct=%.2f\n",
        g_io_threads,
        g_storm_threads,
        accepted / secs,
        us_per_accept,
        alloc_ns,
        us_per_accept > 0 ? alloc_ns / 1000.0 / us_per_accept * 100.0 : 0.0);
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    BenchOptions o;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto next = [&]() -> std::string
        {
            if (i + 1 >= argc)
                Die("参数缺少取值");
            return argv[++i];
        };
        if (a == "--impl")
            o.impl = next();
        else if (a == "--transport")
            o.transport = next();
        else if (a == "--size")
            o.size = static_cast<std::size_t>(std::atoll(next().c_str()));
        else if (a == "--conns")
            o.conns = std::atoi(next().c_str());
        else if (a == "--duration")
            o.duration_s = std::atoi(next().c_str());
        else if (a == "--warmup")
            o.warmup_s = std::atoi(next().c_str());
        else if (a == "--client-threads")
            o.client_threads = std::atoi(next().c_str());
        else if (a == "--server-cpu")
            o.server_cpu = std::atoi(next().c_str());
        else if (a == "--mainloop-cpu")
            o.mainloop_cpu = std::atoi(next().c_str());
        else if (a == "--delay-ms")
            o.delay_ms = std::atoi(next().c_str());
        else if (a == "--rate")
            o.rate = std::atof(next().c_str());
        else if (a == "--active")
            o.active = std::atoi(next().c_str());
        else if (a == "--fanout")
            g_fanout = std::atoi(next().c_str());
        else if (a == "--storm")
            g_storm_threads = std::atoi(next().c_str());
        else if (a == "--io-threads")
            o.io_threads = g_io_threads = std::atoi(next().c_str());
        else if (a == "--reuse-port")
            o.reuse_port = ParseBool(next());
        else if (a == "--mem-probe")
            o.mem_probe = std::atoi(next().c_str());
        else if (a == "--client-cpus")
            o.client_cpus = ParseCpus(next());
        else
            Die("未知参数");
    }
    REQUIRE(o.conns >= 1 && o.client_threads >= 1 && o.size >= 1);
    if (o.io_threads < 1 || o.io_threads > 256)
        Die("--io-threads 要在 1 到 256 之间");
    if (o.transport == "udp" && std::max(o.conns, o.mem_probe) > kUdpMaxConns)
        Die("UDP 在本机最多约 2.5 万条连接（临时端口范围，见 kScaleThreshold 旁的注释）");
    o.client_threads = std::min(o.client_threads, o.conns);

    // 本进程里同时有服务端与客户端两份 fd；先把软限额提到硬限额。
    rlimit rl{};
    if (::getrlimit(RLIMIT_NOFILE, &rl) == 0)
    {
        rl.rlim_cur = rl.rlim_max;
        ::setrlimit(RLIMIT_NOFILE, &rl);
    }

    // 压测线格式：4 字节大端长度 + payload。
    std::vector<std::byte> frame(4 + o.size);
    for (int k = 0; k < 4; ++k)
        frame[static_cast<std::size_t>(k)] = static_cast<std::byte>(o.size >> (8 * (3 - k)));
    for (std::size_t i = 0; i < o.size; ++i)
        frame[4 + i] = static_cast<std::byte>('a' + i % 26);

    // TCP 监听 0.0.0.0，压测线程分别连 127.0.0.1..4：同一源地址连同一 (目的IP, 目的端口) 的连接数受临时端口范围限制
    // （本机 ip_local_port_range 只有约 4000 个），目的 IP 不同则四元组不同，才能凑够上万条连接。
    std::optional<Endpoint> listen_ep;
    const bool kcp_impl = o.impl == "rudp" || o.impl == "rudp-batched" || o.impl == "rudp-mainloop";
    const bool udp_impl = o.impl == "raw-udp" || kcp_impl;
    const bool secure = o.transport == "tls" || o.transport == "ws" || o.transport == "wss";
    if (o.transport == "tcp" || secure)
        listen_ep = Endpoint::Tcp("0.0.0.0", 0);
    else if (o.transport == "uds")
        listen_ep = Endpoint::Unix("/tmp/breeze_bench_" + std::to_string(::getpid()) + ".sock");
    else if (o.transport == "udp")
        listen_ep =
            Endpoint::Udp("127.0.0.1", 0); // 绑具体地址：rudp 用通配地址在多网卡上回包的源地址不对（见设计 §4.14）
    else
        Die("--transport 只能是 tcp、uds、udp、tls、ws 或 wss");
    REQUIRE(listen_ep.has_value());
    if (udp_impl != (o.transport == "udp"))
        Die("raw-udp / rudp / rudp-batched / rudp-mainloop 只能配 --transport udp，反之亦然");
#if defined(BREEZE_BENCH_NO_TLS)
    if (secure)
        Die("这个构建没有 TLS（BREEZE_BUILD_NET_TLS=OFF），--transport tls / ws / wss 不可用");
#endif
    if (secure && o.impl != "breeze" && o.impl != "breeze-mainloop")
        Die("--transport tls / ws / wss 只支持 --impl breeze（IO 线程回显）与 breeze-mainloop（经 NetModule "
            "与主循环）");
    if (secure && o.mem_probe > 0)
        Die("tls / ws / wss 的 --mem-probe 暂不支持：每条服务端 TLS 连接的内存见 tls_layer_test 的 "
            "TestIdleServerLayerMemory（实测约 3171 字节）");

    if (o.reuse_port && (!kcp_impl || g_storm_threads > 0))
        Die("--reuse-port true 只用于 rudp / rudp-batched / rudp-mainloop 的 UDP 服务端");
    if (g_storm_threads == 0 && !kcp_impl && o.io_threads != 1)
        Die("多 IO 线程只用于 rudp 服务端或 --storm；其它回显基线保持单线程");
    if (g_storm_threads > 0)
        return Storm(o);
    if (g_fanout < 1 || g_fanout > 32)
        Die("--fanout 要在 1 到 32 之间");
    if (g_fanout > 1 && (o.impl != "breeze-mainloop" || o.transport == "udp"))
        Die("--fanout 只配 --impl breeze-mainloop（要经 NetModule 的跨线程 Send），传输 tcp / uds / tls / ws / wss");

    g_table_slots = std::max(o.conns, o.mem_probe) > 60000 ? (1u << 17) : (1u << 16);
    auto server = MakeServer(o);
    const Endpoint ep = server->Start(*listen_ep, o.server_cpu);
    const std::size_t server_io_threads = server->ServerThreads().size();
    if (o.mem_probe > 0)
        return MemProbe(o, ep, *server, kcp_impl);
    std::vector<RUdpTicket> tickets;
    if (kcp_impl)
        tickets = server->IssueTickets(o.conns);

    // UDP 的 payload 不带长度前缀（数据报自带边界）
    std::vector<std::byte> payload(o.size);
    for (std::size_t i = 0; i < o.size; ++i)
        payload[i] = static_cast<std::byte>('a' + i % 26);

    std::vector<std::unique_ptr<WorkerBase>> workers;
    std::vector<std::thread> threads;
    std::atomic<bool> measuring{false};
    std::atomic<bool> stop{false};
    std::atomic<int> connected{0};
    std::atomic<bool> go{false};
    const bool scale = !secure && o.conns > kScaleThreshold;
    for (int t = 0; t < o.client_threads; ++t)
    {
        const int share = o.conns / o.client_threads + (t < o.conns % o.client_threads ? 1 : 0);
        const int cpu = o.client_cpus.empty() ? -1 : o.client_cpus[static_cast<std::size_t>(t) % o.client_cpus.size()];
        int begin = 0; // 本线程第一条连接的序号
        for (int k = 0; k < t; ++k)
            begin += o.conns / o.client_threads + (k < o.conns % o.client_threads ? 1 : 0);
        Endpoint target = ep;
        if (o.transport == "tcp")
            target = *Endpoint::Tcp("127.0.0." + std::to_string(1 + t % 4), ep.port());
        if (secure)
        {
#if !defined(BREEZE_BENCH_NO_TLS)
            // TLS 按证书里的主机名校验（与连的是哪个 IP 无关），所以都连 127.0.0.1；连接数受临时端口范围限制。
            target = *Endpoint::Tcp("127.0.0.1", ep.port());
            workers.push_back(std::make_unique<SecureWorker>(
                target, share, payload, cpu, /*tls=*/o.transport != "ws", /*ws=*/o.transport != "tls"));
#endif
            continue;
        }
        if (o.transport == "udp")
        {
            // 票据按线程切片
            std::vector<RUdpTicket> mine;
            if (kcp_impl)
                mine.assign(tickets.begin() + begin, tickets.begin() + begin + share);
            workers.push_back(
                std::make_unique<UdpWorker>(target, share, payload, cpu, kcp_impl, std::move(mine), o.delay_ms));
            continue;
        }
        workers.push_back(std::make_unique<Worker>(target, share, frame, cpu, begin, scale));
    }
    if (o.rate > 0)
        for (auto& w : workers)
            w->SetPeriod(std::chrono::nanoseconds(static_cast<std::int64_t>(1e9 / o.rate)));
    if (o.active > 0) // 活跃连接均摊到各压测线程
        for (int t = 0; t < o.client_threads; ++t)
            workers[static_cast<std::size_t>(t)]->SetActive(o.active / o.client_threads +
                                                            (t < o.active % o.client_threads ? 1 : 0));
    const auto setup0 = BenchClock::now();
    for (int t = 0; t < o.client_threads; ++t)
    {
        threads.emplace_back(
            [&, t]
            {
                workers[static_cast<std::size_t>(t)]->Connect();
                connected.fetch_add(1);
                while (!go.load())
                    std::this_thread::yield();
                workers[static_cast<std::size_t>(t)]->Run(measuring, stop);
            });
    }
    while (connected.load() < o.client_threads)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const double setup_s = std::chrono::duration<double>(BenchClock::now() - setup0).count();
    Phase("clients connected");

    go = true;
    std::this_thread::sleep_for(std::chrono::seconds(o.warmup_s));
    const double cpu0 = ServerCpuSeconds(*server);
    const bool has_extra = server->ExtraThread() != 0;
    const double extra0 = has_extra ? ThreadCpuSeconds(server->ExtraThread()) : 0.0;
    const std::uint64_t iter0 = server->Iterations();
    const std::uint64_t drops0 = UdpRcvbufErrors();
    const std::uint64_t sdrops0 = o.transport == "udp" ? UdpSocketDrops(ep.port()) : 0;
    const std::uint64_t bytes0 = server->BytesIn();
    const std::uint64_t in0 = UdpSnmp("InDatagrams");
    const std::uint64_t out0 = UdpSnmp("OutDatagrams");
    const auto t0 = BenchClock::now();
    measuring = true;
    std::this_thread::sleep_for(std::chrono::seconds(o.duration_s));
    measuring = false;
    const auto t1 = BenchClock::now();
    const double cpu1 = ServerCpuSeconds(*server);
    const double extra1 = has_extra ? ThreadCpuSeconds(server->ExtraThread()) : 0.0;
    const std::uint64_t iters = server->Iterations() - iter0;
    const std::uint64_t drops = UdpRcvbufErrors() - drops0;
    const std::uint64_t sdrops = o.transport == "udp" ? UdpSocketDrops(ep.port()) - sdrops0 : 0;
    const std::uint64_t connected_at_end = server->Connections();
    const double max_lag_us = server->MaxLoopLagUs();
    const std::uint64_t rx_bytes = server->BytesIn() - bytes0;
    const std::uint64_t dgrams_in = UdpSnmp("InDatagrams") - in0;
    const std::uint64_t dgrams_out = UdpSnmp("OutDatagrams") - out0;
    stop = true;
    for (auto& t : threads)
        t.join();
    Phase("clients stopped");
    server->Stop();
    Phase("server stopped");
    if (o.transport == "uds")
        ::unlink(ep.address().c_str());

    std::uint64_t msgs = 0;
    std::uint64_t replied_connections = 0;
    std::vector<std::uint64_t> lat;
    for (auto& w : workers)
    {
        msgs += w->result().msgs;
        replied_connections += w->result().replied_connections;
        lat.insert(lat.end(), w->result().lat_ns.begin(), w->result().lat_ns.end());
    }
    std::sort(lat.begin(), lat.end());
    const double secs = std::chrono::duration<double>(t1 - t0).count();
    const double mps = static_cast<double>(msgs) / secs;
    std::printf(
        "RESULT impl=%s transport=%s size=%zu conns=%d active=%d msgs_per_s=%.0f mb_per_s=%.1f p50_us=%.1f "
        "p99_us=%.1f srv_cpu_pct=%.0f msgs_per_iter=%.1f drops_per_s=%.0f srv_sock_drops_per_s=%.0f "
        "max_lag_us=%.0f srv_rx_bytes_per_msg=%.1f dgram_in_per_msg=%.2f dgram_out_per_msg=%.2f delay_ms=%d "
        "rate=%.1f p90_us=%.1f p999_us=%.1f max_us=%.1f setup_s=%.2f reuse_port=%s io_threads=%zu\n",
        o.impl.c_str(),
        o.transport.c_str(),
        o.size,
        o.conns,
        o.active > 0 ? std::min(o.active, o.conns) : o.conns,
        mps,
        mps * static_cast<double>(o.size) / (1024 * 1024),
        Percentile(lat, 0.50) / 1000.0,
        Percentile(lat, 0.99) / 1000.0,
        (cpu1 - cpu0) / secs * 100.0,
        iters == 0 ? 0.0 : static_cast<double>(msgs) / static_cast<double>(iters),
        static_cast<double>(drops) / secs,
        static_cast<double>(sdrops) / secs,
        max_lag_us,
        msgs == 0 ? 0.0 : static_cast<double>(rx_bytes) / static_cast<double>(msgs),
        msgs == 0 ? 0.0 : static_cast<double>(dgrams_in) / static_cast<double>(msgs),
        msgs == 0 ? 0.0 : static_cast<double>(dgrams_out) / static_cast<double>(msgs),
        o.delay_ms,
        o.rate,
        Percentile(lat, 0.90) / 1000.0,
        Percentile(lat, 0.999) / 1000.0,
        lat.empty() ? 0.0 : static_cast<double>(lat.back()) / 1000.0,
        setup_s,
        o.reuse_port ? "true" : "false",
        server_io_threads);
    std::printf("BENCH_DIAG transport=%s conns=%d active=%d latency_bits=64 "
                "connected_at_end=%llu replied_connections=%llu\n",
                o.transport.c_str(), o.conns, o.active > 0 ? std::min(o.active, o.conns) : o.conns,
                static_cast<unsigned long long>(connected_at_end),
                static_cast<unsigned long long>(replied_connections));
    if (has_extra) // 第二个服务端线程（主循环）的 CPU 占用，单独一行以免打乱 RESULT 的固定字段
        std::printf("EXTRA main_cpu_pct=%.0f\n", (extra1 - extra0) / secs * 100.0);
    return 0;
}
