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
// 压测方式：每条连接闭环 ping-pong（发一帧、等完整回包、再发下一帧），预热后计时。
// 用法：net_bench --impl raw|raw-iov|muduo|breeze|breeze-copy|breeze-fixed|breeze-mainloop --transport tcp|uds --size
// 64 --conns 64
//                 [--duration 3] [--warmup 1] [--client-threads 4] [--server-cpu 2] [--client-cpus 4,6,8,10]
//                 [--mainloop-cpu 12]   breeze-mainloop 的主循环线程绑哪个核（IO 线程用 --server-cpu）

#include <poll.h>
#include <sched.h>
#include <sys/epoll.h>
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
#include <future>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "app.h"
#include "connection.h"
#include "event_loop_thread.h"
#include "group_access.h"
#include "length_prefix_codec.h"
#include "loop_server.h"
#include "net_module.h"
#include "socket.h"
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
    EventLoopThread lt_{EventLoopOptions{EventLoopOptions::kNoGroup, 0, 1u << 16}}; // 连接表要装得下 10k 连接
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
        BizModule(App& a, EchoBiz& biz, std::string listen, std::promise<Endpoint>& ready)
            : Module(a)
            , biz_(biz)
            , listen_(std::move(listen))
            , ready_(ready)
        {
        }
        std::string_view Name() const override { return "biz"; }
        std::vector<std::string_view> DependsOn() const override { return {"net"}; }
        Result<> Init() override
        {
            auto s = app().GetModule<NetModule>("net")->AddServer(listen_, biz_, factory_);
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
        std::string listen_;
        LengthPrefixCodecFactory factory_{1024 * 1024};
        std::promise<Endpoint>& ready_;
        NetServer* server_ = nullptr;
    };

public:
    explicit MainLoopServer(int mainloop_cpu)
        : mainloop_cpu_(mainloop_cpu)
    {
    }

    Endpoint Start(const Endpoint& ep, int cpu) override
    {
        NetModuleOptions no;
        no.group = {1, 1u << 16}; // 连接表要装得下 10k 连接
        app_ = std::make_unique<App>("bench");
        app_->RegisterModule(
            [&](App& a)
            {
                auto m = std::make_unique<NetModule>(a, no);
                net_ = m.get();
                return m;
            });
        std::promise<Endpoint> ready;
        app_->RegisterModule([&](App& a) { return std::make_unique<BizModule>(a, biz_, ep.ToString(), ready); });
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

private:
    int mainloop_cpu_;
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

std::unique_ptr<EchoServer> MakeServer(const std::string& impl, std::size_t size, int mainloop_cpu)
{
    if (impl.rfind("raw", 0) == 0)
    {
        // raw | raw-iov | raw-iov+rdhup | raw-iov+cold | raw-iov+ptr | raw-iov+all（诊断变体，见 RawOptions）
        RawOptions o;
        o.iov = impl.find("-iov") != std::string::npos;
        o.rdhup = impl.find("+rdhup") != std::string::npos || impl.find("+all") != std::string::npos;
        o.cold = impl.find("+cold") != std::string::npos || impl.find("+all") != std::string::npos;
        o.ptr = impl.find("+ptr") != std::string::npos || impl.find("+all") != std::string::npos;
        return std::make_unique<RawServer>(o);
    }
    if (impl == "breeze")
        return std::make_unique<BreezeServer>(std::make_unique<BenchHandler>());
    if (impl == "breeze-copy")
        return std::make_unique<BreezeServer>(std::make_unique<CopyEchoHandler>());
    if (impl == "breeze-fixed")
        return std::make_unique<BreezeServer>(std::make_unique<FixedReplyHandler>(size));
    if (impl == "breeze-mainloop")
        return std::make_unique<MainLoopServer>(mainloop_cpu);
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

class Worker
{
public:
    Worker(const Endpoint& ep, int conns, const std::vector<std::byte>& frame, int cpu)
        : ep_(ep)
        , nconns_(conns)
        , frame_(frame)
        , cpu_(cpu)
    {
    }

    // 连好所有连接（不发数据）。
    void Connect()
    {
        PinTo(cpu_);
        epfd_ = UniqueFd(::epoll_create1(EPOLL_CLOEXEC));
        conns_.resize(static_cast<std::size_t>(nconns_));
        for (int i = 0; i < nconns_; ++i)
        {
            Conn& c = conns_[static_cast<std::size_t>(i)];
            REQUIRE(sock::CreateStreamSocket(ep_, c.fd) == 0);
            int rc;
            // UDS 的非阻塞 connect 在对方 accept 队列满时返回 EAGAIN（TCP 是 SYN
            // 重传，不会走到这里）：等服务端消化一会儿再试。
            while ((rc = sock::Connect(c.fd.get(), ep_)) == EAGAIN)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            REQUIRE(rc == 0 || rc == EINPROGRESS);
            pollfd p{c.fd.get(), POLLOUT, 0};
            REQUIRE(::poll(&p, 1, 10000) > 0 && sock::GetSocketError(c.fd.get()) == 0);
            if (ep_.kind() == Endpoint::Kind::Tcp)
                (void)tcp::SetNoDelay(c.fd.get());
            c.rbuf.resize(frame_.size());
            epoll_event ev{};
            ev.events = EPOLLIN;
            ev.data.u32 = static_cast<std::uint32_t>(i);
            REQUIRE(::epoll_ctl(epfd_.get(), EPOLL_CTL_ADD, c.fd.get(), &ev) == 0);
        }
    }

    // 开始压测，直到 stop。measuring 为 true 期间的完成数与延迟才计入结果。
    void Run(const std::atomic<bool>& measuring, const std::atomic<bool>& stop)
    {
        PinTo(cpu_);
        for (Conn& c : conns_)
            SendNext(c);
        std::vector<epoll_event> evs(1024);
        bool verified_all = false;
        while (!stop.load(std::memory_order_relaxed))
        {
            const int n = ::epoll_wait(epfd_.get(), evs.data(), static_cast<int>(evs.size()), 50);
            for (int i = 0; i < n; ++i)
            {
                Conn& c = conns_[evs[static_cast<std::size_t>(i)].data.u32];
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
                SendNext(c);
            }
            (void)verified_all;
        }
    }

    WorkerResult& result() { return result_; }

private:
    struct Conn
    {
        UniqueFd fd;
        std::vector<std::byte> rbuf;
        std::size_t got = 0;
        bool verified = false;
        BenchClock::time_point sent_at;
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
    UniqueFd epfd_;
    std::vector<Conn> conns_;
    WorkerResult result_;
};

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
        else if (a == "--client-cpus")
            o.client_cpus = ParseCpus(next());
        else
            Die("未知参数");
    }
    REQUIRE(o.conns >= 1 && o.client_threads >= 1 && o.size >= 1);
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
    if (o.transport == "tcp")
        listen_ep = Endpoint::Tcp("0.0.0.0", 0);
    else if (o.transport == "uds")
        listen_ep = Endpoint::Unix("/tmp/breeze_bench_" + std::to_string(::getpid()) + ".sock");
    else
        Die("--transport 只能是 tcp 或 uds");
    REQUIRE(listen_ep.has_value());

    auto server = MakeServer(o.impl, o.size, o.mainloop_cpu);
    const Endpoint ep = server->Start(*listen_ep, o.server_cpu);

    std::vector<std::unique_ptr<Worker>> workers;
    std::vector<std::thread> threads;
    std::atomic<bool> measuring{false};
    std::atomic<bool> stop{false};
    std::atomic<int> connected{0};
    std::atomic<bool> go{false};
    for (int t = 0; t < o.client_threads; ++t)
    {
        const int share = o.conns / o.client_threads + (t < o.conns % o.client_threads ? 1 : 0);
        const int cpu = o.client_cpus.empty() ? -1 : o.client_cpus[static_cast<std::size_t>(t) % o.client_cpus.size()];
        Endpoint target = ep;
        if (o.transport == "tcp")
            target = *Endpoint::Tcp("127.0.0." + std::to_string(1 + t % 4), ep.port());
        workers.push_back(std::make_unique<Worker>(target, share, frame, cpu));
    }
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

    go = true;
    std::this_thread::sleep_for(std::chrono::seconds(o.warmup_s));
    const double cpu0 = ThreadCpuSeconds(server->ServerThread());
    const bool has_extra = server->ExtraThread() != 0;
    const double extra0 = has_extra ? ThreadCpuSeconds(server->ExtraThread()) : 0.0;
    const std::uint64_t iter0 = server->Iterations();
    const auto t0 = BenchClock::now();
    measuring = true;
    std::this_thread::sleep_for(std::chrono::seconds(o.duration_s));
    measuring = false;
    const auto t1 = BenchClock::now();
    const double cpu1 = ThreadCpuSeconds(server->ServerThread());
    const double extra1 = has_extra ? ThreadCpuSeconds(server->ExtraThread()) : 0.0;
    const std::uint64_t iters = server->Iterations() - iter0;
    stop = true;
    for (auto& t : threads)
        t.join();
    server->Stop();
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
        "RESULT impl=%s transport=%s size=%zu conns=%d msgs_per_s=%.0f mb_per_s=%.1f p50_us=%.1f p99_us=%.1f "
        "srv_cpu_pct=%.0f msgs_per_iter=%.1f\n",
        o.impl.c_str(),
        o.transport.c_str(),
        o.size,
        o.conns,
        mps,
        mps * static_cast<double>(o.size) / (1024 * 1024),
        Percentile(lat, 0.50) / 1000.0,
        Percentile(lat, 0.99) / 1000.0,
        (cpu1 - cpu0) / secs * 100.0,
        iters == 0 ? 0.0 : static_cast<double>(msgs) / static_cast<double>(iters));
    if (has_extra) // 第二个服务端线程（主循环）的 CPU 占用，单独一行以免打乱 RESULT 的固定字段
        std::printf("EXTRA main_cpu_pct=%.0f\n", (extra1 - extra0) / secs * 100.0);
    return 0;
}
