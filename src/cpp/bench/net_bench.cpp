// 单 IO 线程 echo 基线（设计见 docs/网络库设计.md §9）。
//
// 被测实现，同一个压测客户端、同一套线格式（4 字节大端长度 + payload）：
//   raw    裸 epoll echo：性能上限，没有任何抽象
//   raw-iov  诊断用：同 raw，但系统调用形状与 breeze 相同（readv 2 个 iovec + sendmsg 2 个 iovec）
//   muduo  muduo 的 TcpServer echo（只支持 TCP）
//   breeze 本库的 ConnectionHandler 直接回调（LoopServer + LengthPrefixCodec），echo 用视图版 Send，不拷贝（与 muduo
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
// 内存探测：--mem-probe N（--impl 任意）：建 N 条空闲连接，报告服务端进程 RSS 的增量 / N（见 MemProbe）。
//
// 压测方式：每条连接闭环 ping-pong（发一帧、等完整回包、再发下一帧），预热后计时。
// 用法：net_bench --impl raw|raw-iov|muduo|breeze|breeze-copy|breeze-fixed|breeze-mainloop --transport tcp|uds --size
// 64 --conns 64
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

#include "app.h"
#include "connection.h"
#include "event_loop_group.h"
#include "event_loop_thread.h"
#include "group_access.h"
#include "kcp_session.h"
#include "length_prefix_codec.h"
#include "loop_server.h"
#include "net_module.h"
#include "rudp_protocol.h"
#include "rudp_server.h"
#include "socket.h"
#include "socket_address.h"
#include "tcp_socket.h"

#ifdef BREEZE_BENCH_MUDUO
#include <muduo/base/Logging.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/TcpServer.h>
#endif

using namespace breeze;

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
    std::vector<int> client_cpus;
};

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
//   存指向每连接对象的指针，而不是 fd（breeze 存 Pollable*）
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
class BenchHandler final : public ConnectionHandler
{
public:
    void OnMessage(Connection& c, std::span<const std::byte> payload) noexcept override { (void)c.Send(payload); }
};

// 旧写法留作参照（breeze-copy）：先拷成 SharedBuffer 再发，每条消息多一次分配与拷贝。
class CopyEchoHandler final : public ConnectionHandler
{
public:
    void OnMessage(Connection& c, std::span<const std::byte> payload) noexcept override
    {
        (void)c.Send(SharedBuffer::CopyFrom(payload));
    }
};

// 诊断变体：回复一块预先建好的固定 payload（压测客户端每次发的内容都相同），不分配、不拷贝。
// 与 breeze 的差就是 echo 里"把收到的 payload 拷成 SharedBuffer"的成本；与 raw
// 的差才是框架本身（分帧、事件分发、帧头）。
class FixedReplyHandler final : public ConnectionHandler
{
public:
    explicit FixedReplyHandler(std::size_t size)
    {
        reply_ = SharedBuffer::Create(size);
        for (std::size_t i = 0; i < size; ++i)
            reply_.MutableBytes()[i] = static_cast<std::byte>('a' + i % 26);
    }
    void OnMessage(Connection& c, std::span<const std::byte>) noexcept override { (void)c.Send(reply_); }

private:
    SharedBuffer reply_;
};

class BreezeServer final : public EchoServer
{
public:
    explicit BreezeServer(std::unique_ptr<ConnectionHandler> handler)
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
    std::uint64_t Iterations() override { return lt_.loop.Iterations(); }

private:
    std::unique_ptr<ConnectionHandler> handler_;
    EventLoopThread lt_{EventLoopOptions{EventLoopOptions::kNoGroup, 0, g_table_slots}}; // 连接表要装得下全部连接
    std::unique_ptr<LengthPrefixCodecFactory> factory_;
    std::unique_ptr<LoopServer> srv_;
};

// 本库 + framework/net：真实的 App::Run 主循环 + NetModule（1 个 IO
// 线程），业务在主循环线程上经句柄把收到的消息原样发回 （SharedBuffer 在 IO
// 线程拷出来、主循环线程直接发回，不再拷）。比 breeze 多出的就是 MainLoopExecutor 的代价。
class MainLoopServer final : public EchoServer
{
    class EchoBiz final : public MainLoopHandler
    {
    public:
        void OnMessage(ConnectionHandle h, SharedBuffer msg) override { (void)h.Send(std::move(msg)); }
    };

    class BizModule final : public Module
    {
    public:
        BizModule(App& a,
                  EchoBiz& biz,
                  const Endpoint& listen,
                  bool rudp,
                  std::promise<Endpoint>& ready,
                  RUdpServer*& rudp_out)
            : Module(a)
            , biz_(biz)
            , listen_(listen)
            , rudp_(rudp)
            , ready_(ready)
            , rudp_out_(rudp_out)
        {
        }
        std::string_view Name() const override { return "biz"; }
        std::vector<std::string_view> DependsOn() const override { return {"net"}; }
        Result<> Init() override
        {
            auto* net = app().GetModule<NetModule>("net");
            if (rudp_)
            {
                // 10 万会话时要一次签发 10 万张票据，超过默认的待用票据上限（65536）
                RUdpServerOptions so;
                so.connection = net->options().connection;
                so.rudp = net->options().rudp;
                so.max_pending_tickets = 1u << 18;
                auto s = net->AddRUdpServer(*RUdpEndpoint::From(listen_), biz_, std::move(so));
                if (!s)
                    return std::unexpected(s.error());
                server_ = *s;
                rudp_out_ = *s; // Start 里的 set_value 把它发布给压测线程
                return {};
            }
            auto s = net->AddServer(listen_.ToString(), biz_, factory_);
            if (!s)
                return std::unexpected(s.error());
            server_ = *s;
            return {};
        }
        Result<> Start() override
        {
            ready_.set_value(server_->endpoint());
            return {};
        }

    private:
        EchoBiz& biz_;
        Endpoint listen_;
        bool rudp_;
        LengthPrefixCodecFactory factory_{1024 * 1024};
        std::promise<Endpoint>& ready_;
        RUdpServer*& rudp_out_;
        NetServer* server_ = nullptr;
    };

public:
    // rudp = true：udp:// 端点（rudp 服务端，flush_on_send），业务照样在主循环线程上回显。
    MainLoopServer(int mainloop_cpu, bool rudp)
        : mainloop_cpu_(mainloop_cpu)
        , rudp_(rudp)
    {
    }

    Endpoint Start(const Endpoint& ep, int cpu) override
    {
        NetModuleOptions no;
        no.group = {1, g_table_slots}; // 连接表要装得下全部连接
        no.rudp.flush_on_send = true; // 只对 udp:// 生效，与 rudp 一样用低延迟配置
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
                             { return std::make_unique<BizModule>(a, biz_, ep, rudp_, ready, rudp_server_); });
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
        EventLoop& loop = GroupAccess::LoopAt(net_->group(), 0);
        std::promise<pthread_t> io;
        loop.Post(
            [&io, cpu]
            {
                PinTo(cpu);
                io.set_value(pthread_self());
            });
        io_thread_ = io.get_future().get();
        return actual;
    }
    void Stop() override
    {
        stop_.request_stop();
        thread_.join();
        // 压测客户端已经全部关闭：不排空，直接销毁（group 析构兜底停 IO 线程）。
        app_.reset();
    }
    pthread_t ServerThread() override { return io_thread_; }
    pthread_t ExtraThread() override { return thread_.native_handle(); }
    std::uint64_t Iterations() override { return GroupAccess::LoopAt(net_->group(), 0).Iterations(); }
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
    RUdpServer* rudp_server_ = nullptr;
    EchoBiz biz_;
    std::unique_ptr<App> app_;
    NetModule* net_ = nullptr;
    std::stop_source stop_;
    Result<> result_;
    std::thread thread_;
    pthread_t io_thread_{};
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
class RUdpEchoHandler final : public ConnectionHandler
{
public:
    void OnMessage(Connection& c, std::span<const std::byte> payload) noexcept override { (void)c.Send(payload); }
};

class RUdpBenchServer final : public EchoServer
{
public:
    explicit RUdpBenchServer(bool flush_on_send)
        : flush_on_send_(flush_on_send)
    {
    }

    Endpoint Start(const Endpoint& ep, int cpu) override
    {
        group_ = std::make_unique<EventLoopGroup>(EventLoopGroupOptions{1, g_table_slots}); // 连接表要装得下全部会话
        REQUIRE(group_->Start() == 0);
        RUdpServerOptions so;
        so.rudp.flush_on_send = flush_on_send_;
        so.max_pending_tickets = 1u << 18; // 10 万会话时要一次签发 10 万张票据，超过默认的 65536
        server_ = std::make_unique<RUdpServer>(*group_, *RUdpEndpoint::From(ep), std::move(so), handler_);
        REQUIRE(server_->Start() == 0);
        EventLoop& loop = GroupAccess::LoopAt(*group_, 0);
        std::promise<pthread_t> io;
        loop.Post(
            [&io, cpu]
            {
                PinTo(cpu);
                io.set_value(pthread_self());
            });
        io_thread_ = io.get_future().get();
        return server_->endpoint();
    }
    void Stop() override
    {
        // 压测客户端已经全部关闭，但服务端会话还在（rudp 没有 TCP 的 RST）：强关再销毁。
        group_->Shutdown(std::chrono::milliseconds::zero());
        server_.reset();
        group_.reset();
    }
    pthread_t ServerThread() override { return io_thread_; }
    std::uint64_t Iterations() override { return GroupAccess::LoopAt(*group_, 0).Iterations(); }
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
    RUdpEchoHandler handler_;
    std::unique_ptr<EventLoopGroup> group_;
    std::unique_ptr<RUdpServer> server_;
    pthread_t io_thread_{};
};

std::unique_ptr<EchoServer> MakeServer(const std::string& impl, std::size_t size, int mainloop_cpu)
{
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
        return std::make_unique<RUdpBenchServer>(true);
    if (impl == "rudp-batched")
        return std::make_unique<RUdpBenchServer>(false);
    if (impl == "breeze")
        return std::make_unique<BreezeServer>(std::make_unique<BenchHandler>());
    if (impl == "breeze-copy")
        return std::make_unique<BreezeServer>(std::make_unique<CopyEchoHandler>());
    if (impl == "breeze-fixed")
        return std::make_unique<BreezeServer>(std::make_unique<FixedReplyHandler>(size));
    if (impl == "breeze-mainloop")
        return std::make_unique<MainLoopServer>(mainloop_cpu, false);
    if (impl == "rudp-mainloop")
        return std::make_unique<MainLoopServer>(mainloop_cpu, true);
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
    std::vector<std::uint32_t> lat_ns;
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
            REQUIRE(::poll(&p, 1, 10000) > 0 && sock::GetSocketError(c.fd.get()) == 0);
            if (target.kind() == Endpoint::Kind::Tcp)
                (void)tcp::SetNoDelay(c.fd.get());
            c.rbuf.resize(frame_.size());
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
                const ssize_t r = ::read(c.fd.get(), c.rbuf.data() + c.got, frame_.size() - c.got);
                if (r <= 0)
                {
                    if (r < 0 && (errno == EAGAIN || errno == EINTR))
                        continue;
                    Die("压测中连接被对端关闭或出错");
                }
                c.got += static_cast<std::size_t>(r);
                if (c.got < frame_.size())
                    continue;
                // 每条连接的第一个回包逐字节核对：别拿一个回显错了的实现去比性能。
                if (!c.verified)
                {
                    REQUIRE(std::memcmp(c.rbuf.data(), frame_.data(), frame_.size()) == 0);
                    c.verified = true;
                }
                if (measuring.load(std::memory_order_relaxed))
                {
                    const auto lat =
                        std::chrono::duration_cast<std::chrono::nanoseconds>(BenchClock::now() - c.sent_at).count();
                    result_.lat_ns.push_back(static_cast<std::uint32_t>(std::min<std::int64_t>(lat, UINT32_MAX)));
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
        bool pending = false; // 有一条已发出、还没收到回包的请求（定速时连接大部分时间在等下一次发送，不是丢包）
        BenchClock::time_point due;     // 定速时：下一次该发送的时刻
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
        }
        if (measuring.load(std::memory_order_relaxed))
        {
            const auto lat =
                std::chrono::duration_cast<std::chrono::nanoseconds>(BenchClock::now() - c.sent_at).count();
            result_.lat_ns.push_back(static_cast<std::uint32_t>(std::min<std::int64_t>(lat, UINT32_MAX)));
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

double Percentile(const std::vector<std::uint32_t>& sorted, double p)
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
    const double cpu0 = ThreadCpuSeconds(server.ServerThread());
    std::this_thread::sleep_for(std::chrono::seconds(3));
    const double idle_cpu = (ThreadCpuSeconds(server.ServerThread()) - cpu0) / 3.0 * 100.0;
    std::printf(
        "MEM impl=%s transport=%s conns=%d rss_before_kb=%ld rss_after_kb=%ld bytes_per_conn=%.0f "
        "idle_cpu_pct=%.2f\n",
        o.impl.c_str(),
        o.transport.c_str(),
        o.mem_probe,
        before,
        after,
        static_cast<double>(after - before) * 1024.0 / o.mem_probe,
        idle_cpu);
    fds.clear();
    server.Stop();
    if (o.transport == "uds")
        ::unlink(ep.address().c_str());
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
        else if (a == "--mem-probe")
            o.mem_probe = std::atoi(next().c_str());
        else if (a == "--client-cpus")
            o.client_cpus = ParseCpus(next());
        else
            Die("未知参数");
    }
    REQUIRE(o.conns >= 1 && o.client_threads >= 1 && o.size >= 1);
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
    if (o.transport == "tcp")
        listen_ep = Endpoint::Tcp("0.0.0.0", 0);
    else if (o.transport == "uds")
        listen_ep = Endpoint::Unix("/tmp/breeze_bench_" + std::to_string(::getpid()) + ".sock");
    else if (o.transport == "udp")
        listen_ep =
            Endpoint::Udp("127.0.0.1", 0); // 绑具体地址：rudp 用通配地址在多网卡上回包的源地址不对（见设计 §4.14）
    else
        Die("--transport 只能是 tcp、uds 或 udp");
    REQUIRE(listen_ep.has_value());
    if (udp_impl != (o.transport == "udp"))
        Die("raw-udp / rudp / rudp-batched / rudp-mainloop 只能配 --transport udp，反之亦然");

    g_table_slots = std::max(o.conns, o.mem_probe) > 60000 ? (1u << 17) : (1u << 16);
    auto server = MakeServer(o.impl, o.size, o.mainloop_cpu);
    const Endpoint ep = server->Start(*listen_ep, o.server_cpu);
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
    const bool scale = o.conns > kScaleThreshold;
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
    const double cpu0 = ThreadCpuSeconds(server->ServerThread());
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
    const double cpu1 = ThreadCpuSeconds(server->ServerThread());
    const double extra1 = has_extra ? ThreadCpuSeconds(server->ExtraThread()) : 0.0;
    const std::uint64_t iters = server->Iterations() - iter0;
    const std::uint64_t drops = UdpRcvbufErrors() - drops0;
    const std::uint64_t sdrops = o.transport == "udp" ? UdpSocketDrops(ep.port()) - sdrops0 : 0;
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
    std::vector<std::uint32_t> lat;
    for (auto& w : workers)
    {
        msgs += w->result().msgs;
        lat.insert(lat.end(), w->result().lat_ns.begin(), w->result().lat_ns.end());
    }
    std::sort(lat.begin(), lat.end());
    const double secs = std::chrono::duration<double>(t1 - t0).count();
    const double mps = static_cast<double>(msgs) / secs;
    std::printf(
        "RESULT impl=%s transport=%s size=%zu conns=%d active=%d msgs_per_s=%.0f mb_per_s=%.1f p50_us=%.1f "
        "p99_us=%.1f srv_cpu_pct=%.0f msgs_per_iter=%.1f drops_per_s=%.0f srv_sock_drops_per_s=%.0f "
        "max_lag_us=%.0f srv_rx_bytes_per_msg=%.1f dgram_in_per_msg=%.2f dgram_out_per_msg=%.2f delay_ms=%d "
        "rate=%.1f p90_us=%.1f p999_us=%.1f max_us=%.1f setup_s=%.2f\n",
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
        setup_s);
    if (has_extra) // 第二个服务端线程（主循环）的 CPU 占用，单独一行以免打乱 RESULT 的固定字段
        std::printf("EXTRA main_cpu_pct=%.0f\n", (extra1 - extra0) / secs * 100.0);
    return 0;
}
