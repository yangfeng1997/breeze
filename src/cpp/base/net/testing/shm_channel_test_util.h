// 共享内存通道测试的公共件：载荷生成与核对、回显处理器、服务端环境、客户端收发、原始客户端。
// 被 shm_channel_test（握手 / 配额 / 协议滥用 / 假 Agent）与 shm_retention_test（保留 / 恢复 / Reattach）共用。
//
// 无测试框架依赖：失败即 abort。

#pragma once

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "base/net/loop/group_access.h"
#include "base/net/shm/shm_channel_client.h"
#include "base/net/shm/shm_channel_server.h"
#include "base/net/shm/shm_protocol.h"
#include "base/net/stream/internal/fd_passing.h"
#include "base/net/testing/net_test_util.h"
#include "base/shm/shm_memfd.h"

namespace shmtest
{

using namespace breeze;
using namespace breeze::testing;
using namespace std::chrono_literals;
namespace wire = breeze::shm::wire;

// ---- 载荷：前 8 字节是序号，其余按序号生成的确定性字节 ----

inline void Fill(std::span<std::byte> p, std::uint64_t seq)
{
    CHECK(p.size() >= 8);
    std::memcpy(p.data(), &seq, 8);
    for (std::size_t i = 8; i < p.size(); ++i)
        p[i] = static_cast<std::byte>((seq * 131 + i * 7 + (seq >> 8)) & 0xFF);
}

inline bool Verify(const std::byte* p, std::uint32_t len, std::uint64_t* seq_out)
{
    if (len < 8)
        return false;
    std::uint64_t seq;
    std::memcpy(&seq, p, 8);
    for (std::uint32_t i = 8; i < len; ++i)
    {
        if (p[i] != static_cast<std::byte>((seq * 131 + i * 7 + (seq >> 8)) & 0xFF))
            return false;
    }
    *seq_out = seq;
    return true;
}

inline std::uint32_t LenOf(std::uint64_t seq) { return 8 + static_cast<std::uint32_t>((seq * 37) % 300); }

// ---- 服务端处理器：回显，并记录事件 ----

inline void SpinFor(int us)
{
    if (us <= 0)
        return;
    const auto end = std::chrono::steady_clock::now() + std::chrono::microseconds(us);
    while (std::chrono::steady_clock::now() < end)
    {
    }
}

class EchoHandler final : public ShmChannelHandler
{
public:
    struct Admit
    {
        std::string busid;
        PeerCredentials peer;
        std::int32_t claimed_pid;
        bool reattach;
        bool resuming;
        bool discard;
    };

    std::atomic<int> admit_deny{0};
    std::atomic<int> budget{64};
    // 处理每条记录之前先 MarkInflight：被 kill -9 时新 Agent 靠它跳过"处理到一半"的那一条（设计 §6.5）。
    bool use_inflight = false;
    // 在"标记之后、提交之前"与"提交之后、Pop 之前"各忙等这么多微秒：把 kill -9 的落点撑大到这两个窗口里
    // （只给跨进程的崩溃用例用，平时是 0）。
    int window_spin_us = 0;
    // 为真时 OnReadable 什么都不处理、一直说"还有"（模拟排不空的通道）。
    std::atomic<bool> stall_readable{false};
    // 每次 OnReadable 开头在 IO 线程上调用：测试用它把 IO 线程卡住，造出"几件事同时就绪"的局面。
    std::function<void()> readable_hook;
    std::atomic<std::uint64_t> readable_calls{0};
    std::atomic<std::uint64_t> echoed{0};
    std::atomic<std::uint64_t> corrupt_seen{0};
    // 同一轮事件循环里 OnReadable 被连续调用的最多次数：用来证明预算用完时库确实让出了（只在设了 loop 时统计）。
    const EventLoop* loop = nullptr;
    std::atomic<int> max_calls_in_iteration{0};

    int OnAdmit(const ShmAdmitRequest& r) noexcept override
    {
        {
            const std::lock_guard lk(mu_);
            admits_.push_back({std::string(r.busid), r.peer, r.claimed_pid, r.reattach, r.resuming, r.discard_retained});
        }
        return admit_deny.load();
    }

    void OnOpened(ShmServerChannel& c) noexcept override
    {
        const std::lock_guard lk(mu_);
        opened_.push_back(c.busid());
        if (c.reattached())
            ++reattached_opens_;
        live_[c.id()] = &c;
        events_.push_back({Event::Kind::Opened, c.id(), c.busid(), {}, {}});
    }

    void OnDetached(ShmServerChannel& c, std::chrono::milliseconds retain) noexcept override
    {
        const std::lock_guard lk(mu_);
        events_.push_back({Event::Kind::Detached, c.id(), c.busid(), retain, {}});
    }

    void OnResumed(ShmServerChannel& c) noexcept override
    {
        const std::lock_guard lk(mu_);
        events_.push_back({Event::Kind::Resumed, c.id(), c.busid(), {}, {}});
    }

    bool OnReadable(ShmServerChannel& c) noexcept override
    {
        readable_calls.fetch_add(1);
        if (readable_hook)
            readable_hook();
        if (stall_readable.load())
            return true;
        if (loop != nullptr)
        {
            const std::uint64_t it = loop->Iterations();
            calls_in_iteration_ = (it == last_iteration_) ? calls_in_iteration_ + 1 : 1;
            last_iteration_ = it;
            if (calls_in_iteration_ > max_calls_in_iteration.load())
                max_calls_in_iteration.store(calls_in_iteration_);
        }
        for (int left = budget.load(); left > 0; --left)
        {
            shm::RecordView v;
            const shm::PeekStatus st = c.tx().Peek(v);
            if (st == shm::PeekStatus::kEmpty)
                return false;
            if (st == shm::PeekStatus::kCorrupt)
            {
                corrupt_seen.fetch_add(1);
                c.Close();
                return false;
            }
            auto r = c.rx().TryReserve(v.len);
            if (r.status == shm::ReserveStatus::kFull)
                return true; // Rx 满了：不 Pop，让出，下一轮再试（此时还没有标记）
            if (use_inflight)
                c.tx().MarkInflight(v);
            SpinFor(window_spin_us);
            CHECK(r.status == shm::ReserveStatus::kOk);
            std::memcpy(r.payload.data(), v.payload, v.len);
            c.rx().Commit();
            SpinFor(window_spin_us);
            c.tx().Pop();
            echoed.fetch_add(1);
        }
        return true;
    }

    void OnClosed(ShmServerChannel& c, ShmCloseReason reason) noexcept override
    {
        const std::lock_guard lk(mu_);
        closed_[c.busid()] = reason;
        ++closed_count_;
        live_.erase(c.id());
        events_.push_back({Event::Kind::Closed, c.id(), c.busid(), {}, reason});
    }

    // 按发生顺序记录的通道事件，用来核对 Opened → Detached → Resumed → Closed 的次序。
    struct Event
    {
        enum class Kind : std::uint8_t
        {
            Opened,
            Detached,
            Resumed,
            Closed
        };
        Kind kind;
        std::uint64_t id;
        std::string busid;
        std::chrono::milliseconds retain;
        std::optional<ShmCloseReason> reason;
    };
    std::vector<Event> events() const
    {
        const std::lock_guard lk(mu_);
        return events_;
    }
    // 某个 busid 的事件序列（Kind 的字符串，例如 "ODR"）。
    std::string trace(const std::string& busid) const
    {
        const std::lock_guard lk(mu_);
        std::string t;
        for (const Event& e : events_)
        {
            if (e.busid != busid)
                continue;
            t += "ODRC"[static_cast<int>(e.kind)];
        }
        return t;
    }
    std::size_t reattached_opens() const
    {
        const std::lock_guard lk(mu_);
        return reattached_opens_;
    }
    // 此刻还活着的、某个 busid 的通道（只能在 IO 线程上用它，见 Env::OnLoop）。
    ShmServerChannel* FindLive(const std::string& busid) const
    {
        const std::lock_guard lk(mu_);
        ShmServerChannel* found = nullptr;
        for (const auto& [id, c] : live_)
        {
            if (c->busid() == busid && (found == nullptr || id > found->id()))
                found = c;
        }
        return found;
    }

    std::vector<Admit> admits() const
    {
        const std::lock_guard lk(mu_);
        return admits_;
    }
    std::size_t opened_count() const
    {
        const std::lock_guard lk(mu_);
        return opened_.size();
    }
    std::size_t closed_count() const
    {
        const std::lock_guard lk(mu_);
        return closed_count_;
    }
    std::optional<ShmCloseReason> closed_reason(const std::string& busid) const
    {
        const std::lock_guard lk(mu_);
        const auto it = closed_.find(busid);
        if (it == closed_.end())
            return std::nullopt;
        return it->second;
    }

private:
    std::uint64_t last_iteration_ = ~0ULL; // 只在 IO 线程上访问
    int calls_in_iteration_ = 0;
    mutable std::mutex mu_;
    std::vector<Admit> admits_;
    std::vector<std::string> opened_;
    std::unordered_map<std::string, ShmCloseReason> closed_;
    std::size_t closed_count_ = 0;
    std::size_t reattached_opens_ = 0;
    std::unordered_map<std::uint64_t, ShmServerChannel*> live_;
    std::vector<Event> events_;
};

// 不保留：App 的 UDS 一断就排空 Tx、销毁通道（保留之前的行为）。握手 / 配额类的用例不关心保留，用它。
inline ShmServerOptions NoRetention()
{
    ShmServerOptions o;
    o.retain_default = 0ms;
    return o;
}

// 服务端 + 它的 group + 临时目录。析构顺序：服务端先停（group 还在跑，同步拆掉全部通道），再 group。
struct Env
{
    // 声明顺序就是销毁顺序的反面：处理器与帧格式要比 group 活得久（group 析构时还会关连接、回调处理器）。
    TempDir dir;
    EchoHandler handler;
    // 仅 UDS 的对端的处理器与帧格式（with_uds 为真时交给服务端）。
    EventRecorder uds_events;
    LengthPrefixCodecFactory uds_codec;
    EventLoopGroup group;
    Endpoint ep;
    std::unique_ptr<ShmChannelServer> server;

    explicit Env(ShmServerOptions opts = NoRetention(), bool with_uds = false)
        : group(EventLoopGroupOptions{1, 256})
        , ep(dir.Unix("agent.sock"))
    {
        Start(std::move(opts), with_uds);
    }
    // 在已有的端点（比如上一个 Env 的）上起服务端：模拟 Agent 重启。
    Env(ShmServerOptions opts, const Endpoint& existing, bool with_uds = false)
        : group(EventLoopGroupOptions{1, 256})
        , ep(existing)
    {
        Start(std::move(opts), with_uds);
    }

    void Start(ShmServerOptions opts, bool with_uds)
    {
        CHECK(group.Start() == 0);
        ShmUdsDataPath path;
        if (with_uds)
        {
            path.handler = &uds_events;
            path.factory = &uds_codec;
        }
        server = std::make_unique<ShmChannelServer>(group, ep, std::move(opts), handler, path);
        CHECK(server->Start() == 0);
    }

    // 在服务端的 IO 线程上执行 fn（通道对象只能在那里碰）。
    template <typename Fn>
    auto OnLoop(Fn&& fn)
    {
        return GroupAccess::PickLoop(group).CallSync(std::forward<Fn>(fn));
    }

    // Agent 一侧往某个通道的 Rx 环写一条消息（载荷按 Fill 生成）。返回写进去没有（通道不在 / 环满了是 false）。
    bool AgentPush(const std::string& busid, std::uint64_t seq, std::uint32_t len)
    {
        return OnLoop(
            [&]
            {
                ShmServerChannel* c = handler.FindLive(busid);
                if (c == nullptr)
                    return false;
                auto r = c->rx().TryReserve(len);
                if (r.status != shm::ReserveStatus::kOk)
                    return false;
                Fill(r.payload, seq);
                c->rx().Commit();
                return true;
            });
    }
};

inline ShmClientOptions ClientOpts(const Endpoint& ep, std::string busid)
{
    ShmClientOptions o;
    o.endpoint = ep;
    o.busid = std::move(busid);
    o.timeout = 5s;
    o.tx_capacity = shm::kMinRingCapacity; // 测试用小环：1 MiB，省内存、建得快
    o.rx_capacity = shm::kMinRingCapacity;
    return o;
}

inline ShmChannelClient MustConnect(const Endpoint& ep, std::string busid)
{
    auto c = ShmChannelClient::Connect(ClientOpts(ep, std::move(busid)));
    if (!c.has_value())
        std::fprintf(stderr, "Connect 失败 errno=%d\n", c.error());
    CHECK(c.has_value());
    return std::move(*c);
}

inline int ConnectErr(const Endpoint& ep, std::string busid, std::uint64_t tx_cap = shm::kMinRingCapacity)
{
    ShmClientOptions o = ClientOpts(ep, std::move(busid));
    o.tx_capacity = tx_cap;
    auto c = ShmChannelClient::Connect(o);
    CHECK(!c.has_value());
    return c.error();
}

// ---- 客户端一侧的收发（睡前协议：App 主循环里怎么用，这里就怎么用）----

inline bool RecvOne(ShmChannelClient& c, shm::RecordView& v, std::chrono::milliseconds timeout = 5s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;)
    {
        if (c.rx().Peek(v) == shm::PeekStatus::kOk)
            return true;
        if (!c.rx().PrepareToSleep())
            continue; // 睡前回头一看又有了
        const auto left =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        pollfd pfd{c.rx().eventfd(), POLLIN, 0};
        const int n = ::poll(&pfd, 1, left.count() > 0 ? static_cast<int>(left.count()) : 0);
        c.rx().WokeUp();
        if (n == 0)
            return c.rx().Peek(v) == shm::PeekStatus::kOk; // 超时前最后看一眼
    }
}

inline bool SendMsg(ShmChannelClient& c, std::uint64_t seq, std::uint32_t len)
{
    auto r = c.tx().TryReserve(len);
    if (r.status != shm::ReserveStatus::kOk)
        return false;
    Fill(r.payload, seq);
    c.tx().Commit();
    return true;
}

// 发 n 条、收 n 条回显，按窗口交替；核对顺序、长度与内容。
inline void EchoRoundTrips(ShmChannelClient& c, std::uint64_t n, std::uint64_t first_seq = 0, std::uint64_t window = 32)
{
    std::uint64_t sent = 0, recvd = 0;
    while (recvd < n)
    {
        while (sent < n && sent - recvd < window)
        {
            CHECK(SendMsg(c, first_seq + sent, LenOf(first_seq + sent)));
            ++sent;
        }
        shm::RecordView v;
        CHECK(RecvOne(c, v));
        std::uint64_t seq = 0;
        CHECK(Verify(v.payload, v.len, &seq));
        CHECK(seq == first_seq + recvd && v.len == LenOf(seq));
        c.rx().Pop();
        ++recvd;
    }
}

// ---- 原始客户端：造出各种不规矩的握手 ----

inline UniqueFd RawConnect(const Endpoint& ep)
{
    UniqueFd fd(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    CHECK(fd.valid());
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    std::strncpy(a.sun_path, ep.address().c_str(), sizeof(a.sun_path) - 1);
    CHECK(::connect(fd.get(), reinterpret_cast<const sockaddr*>(&a), sizeof(a)) == 0);
    return fd;
}

inline wire::Hello MakeHello(std::string_view busid = "raw")
{
    wire::Hello h{};
    h.hdr = wire::MakeHeader(wire::MsgType::kHello);
    h.attach_type = static_cast<std::uint32_t>(wire::AttachType::kAttach);
    h.transport = static_cast<std::uint32_t>(wire::Transport::kShmRequired);
    h.pid = static_cast<std::int32_t>(::getpid());
    h.tx_capacity = shm::kMinRingCapacity;
    h.rx_capacity = shm::kMinRingCapacity;
    CHECK(wire::SetBusId(h, busid));
    return h;
}

inline void RawSend(int fd, const void* p, std::size_t n) { CHECK(::send(fd, p, n, MSG_NOSIGNAL) == static_cast<ssize_t>(n)); }

struct RawReply
{
    wire::Reply reply{};
    std::size_t nfds = 0;
    std::array<UniqueFd, 4> fds;
};

// 读一条 Reply（连同随它附带的 fd）。超时或读到 EOF 返回空。
inline std::optional<RawReply> RawRecvReply(int fd, std::chrono::milliseconds timeout = 5s)
{
    RawReply out;
    std::array<std::byte, sizeof(wire::Reply)> buf{};
    std::size_t got = 0;
    while (got < buf.size())
    {
        pollfd pfd{fd, POLLIN, 0};
        if (::poll(&pfd, 1, static_cast<int>(timeout.count())) <= 0)
            return std::nullopt;
        std::array<UniqueFd, 4> chunk;
        std::size_t n = 0, nf = 0;
        if (sock::RecvWithFds(fd, std::span(buf).subspan(got), n, chunk, nf) != 0 || n == 0)
            return std::nullopt;
        got += n;
        for (std::size_t i = 0; i < nf; ++i)
            out.fds[out.nfds++] = std::move(chunk[i]);
    }
    std::memcpy(&out.reply, buf.data(), sizeof(out.reply));
    return out;
}

// 对端关闭了吗（读到 EOF）。
inline bool RawSeesEof(int fd, std::chrono::milliseconds timeout = 5s)
{
    pollfd pfd{fd, POLLIN, 0};
    if (::poll(&pfd, 1, static_cast<int>(timeout.count())) <= 0)
        return false;
    char c;
    return ::recv(fd, &c, 1, MSG_DONTWAIT) == 0;
}

inline int RawHandshake(const Endpoint& ep, const wire::Hello& hello)
{
    UniqueFd fd = RawConnect(ep);
    RawSend(fd.get(), &hello, sizeof(hello));
    auto r = RawRecvReply(fd.get());
    CHECK(r.has_value());
    CHECK(wire::CheckReplyHeader(r->reply) == 0);
    if (r->reply.result != 0)
        CHECK(r->nfds == 0); // Reject 不带 fd
    return r->reply.result;
}

// ---- 跨进程 ----

struct Child
{
    pid_t pid = -1;
    int go_fd = -1; // 父进程往这里写一个字节，子进程才开始
};

inline Child SpawnClient(std::function<int()> body)
{
    int p[2];
    CHECK(::pipe(p) == 0);
    const pid_t pid = ::fork();
    CHECK(pid >= 0);
    if (pid == 0)
    {
        ::close(p[1]);
        ::alarm(60);
        char c;
        if (::read(p[0], &c, 1) != 1)
            ::_exit(90);
        ::_exit(body());
    }
    ::close(p[0]);
    return {pid, p[1]};
}

inline void Go(const Child& c) { CHECK(::write(c.go_fd, "g", 1) == 1); }

inline void ExpectExit0(const Child& c, const char* what)
{
    int st = 0;
    CHECK(::waitpid(c.pid, &st, 0) == c.pid);
    if (!(WIFEXITED(st) && WEXITSTATUS(st) == 0))
        std::fprintf(stderr, "子进程 [%s] 异常结束 status=0x%x\n", what, st);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

// 一块新建的、没有任何人用过的通道内存（假 Agent / 假 App 用）。
inline shm::ShmChannel NewShm(std::uint64_t cap = shm::kMinRingCapacity)
{
    shm::ChannelOptions o;
    o.tx_capacity = cap;
    o.rx_capacity = cap;
    auto ch = shm::ShmChannel::Create(o);
    CHECK(ch.has_value());
    return std::move(*ch);
}

// ---- 假 Agent（只会回复，不是真的服务端）----

class FakeAgent
{
public:
    using Behavior = std::function<void(int conn, const wire::Hello&)>;

    FakeAgent(const Endpoint& ep, Behavior behavior)
        : FakeAgent(ep, std::vector<Behavior>{std::move(behavior)})
    {
    }

    // 按顺序处理 behaviors.size() 个连接（比如：先握手成功，再回一个坏的 Reattach 回复）。
    FakeAgent(const Endpoint& ep, std::vector<Behavior> behaviors)
        : behaviors_(std::move(behaviors))
    {
        listen_.reset(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
        CHECK(listen_.valid());
        sockaddr_un a{};
        a.sun_family = AF_UNIX;
        std::strncpy(a.sun_path, ep.address().c_str(), sizeof(a.sun_path) - 1);
        CHECK(::bind(listen_.get(), reinterpret_cast<const sockaddr*>(&a), sizeof(a)) == 0);
        CHECK(::listen(listen_.get(), 4) == 0);
        thread_ = std::thread(
            [this]
            {
                for (const Behavior& behavior : behaviors_)
                {
                    UniqueFd conn(::accept4(listen_.get(), nullptr, nullptr, SOCK_CLOEXEC));
                    CHECK(conn.valid());
                    wire::Hello h{};
                    // 不用 recvmsg：Reattach 的 Hello 带的 fd 被内核丢弃，假 Agent 用不着它们。
                    CHECK(::recv(conn.get(), &h, sizeof(h), MSG_WAITALL) == static_cast<ssize_t>(sizeof(h)));
                    {
                        const std::lock_guard lk(mu_);
                        hello_ = h;
                        hellos_.push_back(h);
                    }
                    behavior(conn.get(), h);
                }
            });
    }
    ~FakeAgent() { thread_.join(); }

    wire::Hello hello() const
    {
        const std::lock_guard lk(mu_);
        return hello_;
    }
    std::vector<wire::Hello> hellos() const
    {
        const std::lock_guard lk(mu_);
        return hellos_;
    }

private:
    std::vector<Behavior> behaviors_;
    UniqueFd listen_;
    std::thread thread_;
    mutable std::mutex mu_;
    wire::Hello hello_{};
    std::vector<wire::Hello> hellos_;
};

inline wire::Reply GoodReply(const shm::ShmChannel& ch)
{
    const shm::ChannelLayout& l = ch.layout();
    wire::Reply r{};
    r.hdr = wire::MakeHeader(wire::MsgType::kReply);
    r.result = 0;
    r.app_epoch = 1;
    r.agent_epoch = 1;
    r.total_size = l.total_size;
    r.tx = {l.tx.capacity, l.tx.max_payload, 0};
    r.rx = {l.rx.capacity, l.rx.max_payload, 0};
    r.transport = static_cast<std::uint32_t>(wire::ChosenTransport::kShm);
    return r;
}

inline void SendReply(int conn, const wire::Reply& r, std::span<const int> fds)
{
    CHECK(sock::SendWithFds(conn, std::as_bytes(std::span(&r, 1)), fds) == 0);
}

// 跑一次：假 Agent 按 behavior 回复，返回客户端 Connect 的结果（成功时是 0）。
inline int ConnectToFake(const std::function<void(int, const wire::Hello&)>& behavior,
                  std::chrono::milliseconds timeout = 5s,
                  shm::AttachLimits limits = {},
                  wire::Hello* seen = nullptr)
{
    TempDir dir;
    const Endpoint ep = dir.Unix("fake.sock");
    FakeAgent fake(ep, behavior);
    ShmClientOptions o = ClientOpts(ep, "fake-app");
    o.timeout = timeout;
    o.limits = limits;
    auto r = ShmChannelClient::Connect(o);
    const int result = r.has_value() ? 0 : r.error();
    r = std::unexpected(0); // 先放掉客户端（关 UDS），假 Agent 的线程才会结束
    if (seen != nullptr)
        *seen = fake.hello();
    return result;
}

// ---- 握手之后的原始连接，与长度前缀的帧（仅 UDS 的数据通道、路由器的测试用）----

using Transport = wire::Transport;


// 握手之后的原始连接：回复里的 result 可能不是 0（被拒）。
struct RawConn
{
    UniqueFd fd;
    RawReply reply;
    int result() const { return reply.reply.result; }
    bool uds() const { return reply.reply.transport == static_cast<std::uint32_t>(wire::ChosenTransport::kUds); }
    bool shm() const { return reply.reply.transport == static_cast<std::uint32_t>(wire::ChosenTransport::kShm); }
};

inline RawConn Handshake(const Endpoint& ep,
                  std::string_view busid,
                  Transport tr,
                  std::uint32_t flags = 0,
                  std::uint64_t tx_cap = shm::kMinRingCapacity)
{
    RawConn c;
    c.fd = RawConnect(ep);
    wire::Hello h = MakeHello(busid);
    h.transport = static_cast<std::uint32_t>(tr);
    h.flags = flags;
    h.tx_capacity = tx_cap;
    RawSend(c.fd.get(), &h, sizeof(h));
    auto r = RawRecvReply(c.fd.get());
    CHECK(r.has_value() && wire::CheckReplyHeader(r->reply) == 0);
    c.reply = std::move(*r);
    return c;
}

// 长度前缀：4 字节大端 + 载荷（LengthPrefixCodec 的格式）。
inline void SendFrame(int fd, std::string_view payload)
{
    LengthPrefixCodec codec(1 << 20);
    std::array<std::byte, kMaxFrameHeaderSize> hdr{};
    const std::size_t n = codec.EncodeHeader(payload.size(), hdr);
    CHECK(n == 4);
    std::string wire_bytes(reinterpret_cast<const char*>(hdr.data()), n);
    wire_bytes.append(payload);
    RawSend(fd, wire_bytes.data(), wire_bytes.size());
}

inline bool ReadExact(int fd, void* buf, std::size_t n, std::chrono::milliseconds timeout)
{
    auto* p = static_cast<char*>(buf);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (n > 0)
    {
        const auto left =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        pollfd pfd{fd, POLLIN, 0};
        if (left.count() <= 0 || ::poll(&pfd, 1, static_cast<int>(left.count())) <= 0)
            return false;
        const ssize_t got = ::recv(fd, p, n, 0);
        if (got <= 0)
            return false;
        p += got;
        n -= static_cast<std::size_t>(got);
    }
    return true;
}

inline std::optional<std::string> RecvFrame(int fd, std::chrono::milliseconds timeout = 5s)
{
    std::array<unsigned char, 4> hdr{};
    if (!ReadExact(fd, hdr.data(), hdr.size(), timeout))
        return std::nullopt;
    const std::size_t len = (std::size_t{hdr[0]} << 24) | (std::size_t{hdr[1]} << 16) | (std::size_t{hdr[2]} << 8) | hdr[3];
    std::string out(len, '\0');
    if (len > 0 && !ReadExact(fd, out.data(), len, timeout))
        return std::nullopt;
    return out;
}

// 仅 UDS 的对端发什么回什么（在 IO 线程上回包）。
inline void EchoOnUds(Env& env)
{
    env.uds_events.on_message = [](IConnection& c, std::string_view p)
    { (void)c.Send(std::span<const std::byte>(reinterpret_cast<const std::byte*>(p.data()), p.size())); };
}

// ---- 换用户（要 root）----

// 以另一个用户的身份做一件事。Linux 的凭据是线程级的：直接发系统调用，只改这个新线程，不影响别的线程。
// 需要 root；SO_PEERCRED 取的是 connect 那一刻的凭据。
template <typename Fn>
inline auto AsUser(std::uint32_t uid, Fn fn)
{
    std::invoke_result_t<Fn> result{};
    std::thread t(
        [&]
        {
            CHECK(::syscall(SYS_setresuid, uid, uid, uid) == 0);
            result = fn();
        });
    t.join();
    return result;
}

// 让别的用户也连得上这个 UDS（目录与 socket 文件的权限）。
inline void OpenSocketToAll(const Env& env)
{
    CHECK(::chmod(env.dir.path().c_str(), 0755) == 0);
    CHECK(::chmod(env.ep.address().c_str(), 0777) == 0);
}

} // namespace shmtest
