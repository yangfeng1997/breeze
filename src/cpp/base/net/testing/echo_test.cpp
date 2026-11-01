// 无测试框架依赖：失败即 abort。
//
// 同一套用例分别跑在 TCP、UDS 路径、UDS 抽象名上——"同一份代码跑通"是 P2a 的验收点：
// 差异只在建立时，IConnection 以上没有 if tcp。每个用例独占一个 server，结束时检查没有 fd 泄漏。

#include <dirent.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "base/buffer/buffer.h"
#include "base/net/codec/length_prefix_codec.h"
#include "base/net/connection/connection_interface.h"
#include "base/net/loop/event_loop_thread.h"
#include "base/net/stream/internal/loop_server.h"
#include "base/net/stream/internal/stream_socket.h"

using namespace breeze;
using namespace std::chrono_literals;

#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                              \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (0)

namespace
{

constexpr std::size_t kServerMaxFrame = 4 * 1024 * 1024;
constexpr auto kWait = 10s;

// ---- 记录连接事件的 handler ----

const char* ReasonName(CloseReason r)
{
    switch (r)
    {
        case CloseReason::PeerClosed:
            return "PeerClosed";
        case CloseReason::LocalClose:
            return "LocalClose";
        case CloseReason::IdleTimeout:
            return "IdleTimeout";
        case CloseReason::ProtocolError:
            return "ProtocolError";
        case CloseReason::SlowConsumer:
            return "SlowConsumer";
        case CloseReason::IoError:
            return "IoError";
    }
    return "?";
}

struct ConnLog
{
    std::vector<std::string> events; // "C" | "M:<字节数>" | "X:<原因>:<errno>"
    std::optional<Endpoint> peer_endpoint;
    std::optional<PeerCredentials> peer_credentials;
    // 回调里 Close 之后的观察
    bool send_after_close_closed = false;
    bool connected_after_close = true;
};

// 默认回显。触发词让它做别的事，用来构造各种连接行为。
class RecordingHandler : public IConnectionHandler
{
public:
    std::string close_trigger;        // 收到这个 payload：先回显，再 Close，再试着 Send
    std::string flood_trigger;        // 收到这个 payload：连发 kFloodFrames 个大帧
    std::string oversize_trigger;     // 收到这个 payload：发一个超过 codec 上限的帧
    std::string view_big_trigger;     // 收到这个 payload：用视图版 Send 发一个大帧，返回前立刻改写原内存
    std::string view_backlog_trigger; // 同上，但先连发 kFloodFrames 个大帧造成积压，视图版 Send 必须排在它们后面
    bool close_on_connect = false;    // OnConnected 里直接 Close
    bool echo_copy = false;           // 默认回显走视图版 Send（不拷贝）；设为 true 则先拷成 SharedBuffer 再发

    static constexpr int kFloodFrames = 200;
    static constexpr std::size_t kFloodFrameSize = 50 * 1024;
    static constexpr std::size_t kViewBigSize = 3 * 1024 * 1024; // 小于服务端的 max_frame（4MB）

    static char ViewBigByte(std::size_t i) { return static_cast<char>('A' + (i * 7) % 26); }

    void OnConnected(IConnection& c) noexcept override
    {
        {
            const std::lock_guard lk(mu_);
            CHECK(active_.find(&c) == active_.end()); // 同一条连接只有一次 OnConnected
            ConnLog& log = active_[&c];
            log.events.push_back("C");
            log.peer_endpoint = c.peer().endpoint;
            log.peer_credentials = c.peer().credentials;
        }
        connected_.fetch_add(1);
        if (close_on_connect)
            c.Close();
    }

    void OnMessage(IConnection& c, std::span<const std::byte> payload) noexcept override
    {
        const std::string text(reinterpret_cast<const char*>(payload.data()), payload.size());
        {
            const std::lock_guard lk(mu_);
            auto it = active_.find(&c);
            CHECK(it != active_.end());                // OnClosed 之后不能再有 OnMessage
            CHECK(it->second.events.back()[0] != 'X'); // （active_ 里的连接都还没 Closed）
            it->second.events.push_back("M:" + std::to_string(payload.size()));
        }
        if (!oversize_trigger.empty() && text == oversize_trigger)
        {
            CHECK(c.Send(SharedBuffer::Create(2000)) == SendResult::Closed); // 超过 codec 上限：关闭连接
            return;
        }
        if (!flood_trigger.empty() && text == flood_trigger)
        {
            Flood(c);
            return;
        }
        if ((!view_big_trigger.empty() && text == view_big_trigger) ||
            (!view_backlog_trigger.empty() && text == view_backlog_trigger))
        {
            if (text == view_backlog_trigger)
                Flood(c); // 先造成积压：之后的视图版 Send 不能直接写，必须拷贝后排在积压之后
            std::vector<std::byte> big(kViewBigSize);
            for (std::size_t i = 0; i < big.size(); ++i)
                big[i] = static_cast<std::byte>(ViewBigByte(i));
            CHECK(c.Send(std::span<const std::byte>(big)) == SendResult::Ok);
            // 返回之后视图指向的内存可以立刻改写或释放：库对没能写出去的部分必须已经拷走了。这里把它整个毁掉，
            // 客户端收到的还得是原来的内容。
            std::memset(big.data(), 0xEE, big.size());
            return;
        }
        if (echo_copy)
            CHECK(c.Send(SharedBuffer::CopyFrom(payload)) == SendResult::Ok);
        else
            CHECK(c.Send(payload) == SendResult::Ok); // 视图版：直接从读缓冲写回，写不完才拷贝
        if (!close_trigger.empty() && text == close_trigger)
        {
            c.Close();
            // 两个重载在关闭之后都返回 Closed
            const bool closed =
                c.Send(SharedBuffer::CopyFrom(payload)) == SendResult::Closed && c.Send(payload) == SendResult::Closed;
            const bool connected = c.IsConnected();
            c.Close(); // 重复 Close 是空操作
            const std::lock_guard lk(mu_);
            active_[&c].send_after_close_closed = closed;
            active_[&c].connected_after_close = connected;
        }
    }

    static void Flood(IConnection& c)
    {
        for (int i = 0; i < kFloodFrames; ++i)
        {
            SharedBuffer b = SharedBuffer::Create(kFloodFrameSize);
            std::memset(b.MutableBytes().data(), 'a' + i % 26, kFloodFrameSize);
            CHECK(c.Send(std::move(b)) == SendResult::Ok); // 排队也是 Ok
        }
    }

    void OnClosed(IConnection& c, CloseReason r, int err) noexcept override
    {
        const std::lock_guard lk(mu_);
        auto it = active_.find(&c);
        CHECK(it != active_.end()); // 每连接恰一个 OnClosed
        it->second.events.push_back(std::string("X:") + ReasonName(r) + ":" + std::to_string(err));
        finished_.push_back(std::move(it->second));
        active_.erase(it);
    }

    bool WaitConnected(int n) const
    {
        const auto deadline = std::chrono::steady_clock::now() + kWait;
        while (connected_.load() < n)
        {
            if (std::chrono::steady_clock::now() > deadline)
                return false;
            std::this_thread::sleep_for(200us);
        }
        return true;
    }

    // 等到至少 n 条连接已经 OnClosed，返回它们的记录。
    std::vector<ConnLog> WaitFinished(std::size_t n) const
    {
        const auto deadline = std::chrono::steady_clock::now() + kWait;
        for (;;)
        {
            {
                const std::lock_guard lk(mu_);
                if (finished_.size() >= n)
                    return finished_;
            }
            CHECK(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(200us);
        }
    }

    // 等一小会儿再看：用来断言"之后不会再发生任何回调"。
    std::size_t FinishedCountNow() const
    {
        const std::lock_guard lk(mu_);
        return finished_.size();
    }

private:
    mutable std::mutex mu_;
    std::map<IConnection*, ConnLog> active_;
    std::vector<ConnLog> finished_;
    std::atomic<int> connected_{0};
};

// ---- 服务端装配 ----

struct Server
{
    // 水位放到 1GB：这里的用例会故意制造 10MB 级别的写积压，水位本身由 connection_handle_test 覆盖。
    Server(IConnectionHandler& handler, const Endpoint& ep, std::size_t max_frame = kServerMaxFrame)
        : factory(max_frame)
        , srv(lt.loop, ep, handler, factory, ConnectionOptions{1ull << 30})
    {
        CHECK(srv.Start() == 0);
        lt.Start();
    }
    ~Server() { lt.Stop(); } // 先停 loop 线程，之后成员销毁时 Acceptor::Stop 不再与它并发

    const Endpoint& ep() const { return srv.endpoint(); }

    EventLoopThread lt;
    LengthPrefixCodecFactory factory;
    LoopServer srv;
};

// ---- 客户端：阻塞风格的辅助，全部带超时 ----

class Client
{
public:
    explicit Client(const Endpoint& ep)
    {
        CHECK(sock::CreateStreamSocket(ep, fd_) == 0);
        const int rc = sock::Connect(fd_.get(), ep);
        CHECK(rc == 0 || rc == EINPROGRESS);
        CHECK(Wait(POLLOUT, kWait));
        CHECK(sock::GetSocketError(fd_.get()) == 0);
    }

    int fd() const { return fd_.get(); }

    void SendAll(std::span<const std::byte> data)
    {
        std::size_t off = 0;
        while (off < data.size())
        {
            const ssize_t n = ::send(fd_.get(), data.data() + off, data.size() - off, MSG_NOSIGNAL);
            if (n > 0)
                off += static_cast<std::size_t>(n);
            else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                CHECK(Wait(POLLOUT, kWait));
            else
                CHECK(false);
        }
    }

    static std::vector<std::byte> Frame(std::string_view payload)
    {
        LengthPrefixCodec codec(16 * 1024 * 1024);
        std::array<std::byte, kMaxFrameHeaderSize> hdr{};
        const std::size_t n = codec.EncodeHeader(payload.size(), hdr);
        std::vector<std::byte> out(hdr.begin(), hdr.begin() + static_cast<std::ptrdiff_t>(n));
        for (const char c : payload)
            out.push_back(static_cast<std::byte>(c));
        return out;
    }

    void SendFrame(std::string_view payload)
    {
        const auto f = Frame(payload);
        SendAll(f);
    }

    std::string RecvFrame()
    {
        for (;;)
        {
            std::string out;
            std::size_t used = 0;
            const DecodeResult r = codec_.Decode(
                buf_.Readable(),
                [&](std::span<const std::byte> p) { out.assign(reinterpret_cast<const char*>(p.data()), p.size()); },
                used);
            if (r == DecodeResult::Ok)
            {
                buf_.Consume(used);
                return out;
            }
            CHECK(r == DecodeResult::NeedMore);
            buf_.PrepareWrite(64 * 1024);
            const auto w = buf_.Writable();
            const ssize_t n = ::recv(fd_.get(), w.data(), w.size(), 0);
            if (n > 0)
                buf_.Commit(static_cast<std::size_t>(n));
            else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                CHECK(Wait(POLLIN, kWait));
            else
                CHECK(false && "RecvFrame：连接意外关闭");
        }
    }

    // 对端关闭（EOF 或 RST）返回 true；期间收到的数据丢弃。
    bool WaitClosed()
    {
        const auto deadline = std::chrono::steady_clock::now() + kWait;
        char junk[4096];
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (!Wait(POLLIN, 200ms))
                continue;
            const ssize_t n = ::recv(fd_.get(), junk, sizeof(junk), 0);
            if (n == 0)
                return true;
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
                return true; // ECONNRESET 等
        }
        return false;
    }

    // 让随后的 close 发 RST 而不是正常的 FIN。
    void ArmReset()
    {
        const linger lg{1, 0};
        CHECK(::setsockopt(fd_.get(), SOL_SOCKET, SO_LINGER, &lg, sizeof(lg)) == 0);
    }

    void Close() { fd_.reset(); }

private:
    bool Wait(short events, std::chrono::milliseconds timeout)
    {
        pollfd p{fd_.get(), events, 0};
        return ::poll(&p, 1, static_cast<int>(timeout.count())) > 0;
    }

    UniqueFd fd_;
    Buffer buf_;
    LengthPrefixCodec codec_{16 * 1024 * 1024};
};

// 用例前后打开的 fd 数必须相同：没有 fd 泄漏（含服务端的 epoll / eventfd / 连接 fd）。
int CountOpenFds()
{
    int n = 0;
    if (DIR* d = ::opendir("/proc/self/fd"))
    {
        while (::readdir(d) != nullptr)
            ++n;
        ::closedir(d);
    }
    return n;
}

struct FdLeakCheck
{
    int before = CountOpenFds();
    ~FdLeakCheck()
    {
        const int after = CountOpenFds();
        if (after != before)
        {
            std::fprintf(stderr, "fd 泄漏：用例前 %d 个，用例后 %d 个\n", before, after);
            std::abort();
        }
    }
};

// ---- 用例 ----

// 每个用例拿到一个"新鲜的监听端点"：TCP 端口由系统分配，UDS 路径在临时目录里，抽象名带进程号与序号。
struct Target
{
    const char* label;
    std::function<Endpoint()> make;
    bool is_tcp;
};

std::string MakeTmpDir()
{
    char tmpl[] = "/tmp/breeze_echo_XXXXXX";
    CHECK(::mkdtemp(tmpl) != nullptr);
    return tmpl;
}

std::vector<std::string> Ev(std::initializer_list<const char*> l) { return {l.begin(), l.end()}; }

void TestEcho(const Target& t)
{
    const FdLeakCheck leak;
    RecordingHandler h;
    {
        Server s(h, t.make());
        Client c(s.ep());
        for (const std::string& p :
             {std::string("hello"), std::string(), std::string(1000, 'x'), std::string(70000, 'y')})
        {
            c.SendFrame(p);
            CHECK(c.RecvFrame() == p); // 70000 > 64KB 溢出缓冲：覆盖"每连接缓冲 + 线程本地溢出缓冲"两条路径
        }
        c.Close();
        const auto logs = h.WaitFinished(1);
        CHECK((logs[0].events == Ev({"C", "M:5", "M:0", "M:1000", "M:70000", "X:PeerClosed:0"})));
    }
}

// 一帧被拆成许多次到达：分帧器要把半包攒起来。
void TestFrameSplitAcrossReads(const Target& t)
{
    const FdLeakCheck leak;
    RecordingHandler h;
    Server s(h, t.make());
    Client c(s.ep());
    const auto f = Client::Frame("split-across-reads");
    for (std::size_t i = 0; i < f.size(); ++i)
    {
        c.SendAll({&f[i], 1});
        std::this_thread::sleep_for(300us); // 让服务端在这一字节上先读一次
    }
    CHECK(c.RecvFrame() == "split-across-reads");
    // 拼完一个跨读的帧之后再来一帧：回包必须恰好是新帧。拼接路径若漏了消耗，旧帧会被重复分发，这里就会对不上。
    c.SendFrame("second");
    CHECK(c.RecvFrame() == "second");
    c.SendFrame("third");
    CHECK(c.RecvFrame() == "third");
}

// 2MB 的帧：远超线程本地溢出缓冲，回显时又超过内核发送缓冲，走"入队 + EPOLLOUT"的路径。
void TestLargeFrame(const Target& t)
{
    const FdLeakCheck leak;
    RecordingHandler h;
    Server s(h, t.make());
    Client c(s.ep());
    std::string big(2 * 1024 * 1024, '\0');
    for (std::size_t i = 0; i < big.size(); ++i)
        big[i] = static_cast<char>('A' + (i * 7) % 26);
    c.SendFrame(big);
    CHECK(c.RecvFrame() == big);
}

// 不等回包连发 5000 帧：回显的顺序不能乱。
void TestPipelined(const Target& t)
{
    const FdLeakCheck leak;
    RecordingHandler h;
    Server s(h, t.make());
    Client c(s.ep());
    std::vector<std::byte> all;
    for (int i = 0; i < 5000; ++i)
    {
        const auto f = Client::Frame("msg-" + std::to_string(i));
        all.insert(all.end(), f.begin(), f.end());
    }
    c.SendAll(all);
    for (int i = 0; i < 5000; ++i)
        CHECK(c.RecvFrame() == "msg-" + std::to_string(i));
    // 每一帧恰好交付一次：之后再发一帧，回包必须就是它（不能先冒出某个旧帧的重放）。
    c.SendFrame("end");
    CHECK(c.RecvFrame() == "end");
}

void TestManyConnections(const Target& t)
{
    const FdLeakCheck leak;
    RecordingHandler h;
    constexpr int kConns = 64;
    constexpr int kRounds = 20;
    {
        Server s(h, t.make());
        std::vector<std::unique_ptr<Client>> cs;
        for (int i = 0; i < kConns; ++i)
            cs.push_back(std::make_unique<Client>(s.ep()));
        for (int r = 0; r < kRounds; ++r)
        {
            for (int i = 0; i < kConns; ++i)
                cs[static_cast<std::size_t>(i)]->SendFrame("c" + std::to_string(i) + "-r" + std::to_string(r));
            for (int i = 0; i < kConns; ++i)
                CHECK(cs[static_cast<std::size_t>(i)]->RecvFrame() ==
                      "c" + std::to_string(i) + "-r" + std::to_string(r));
        }
        cs.clear();
        const auto logs = h.WaitFinished(kConns);
        for (const ConnLog& log : logs)
        {
            CHECK(log.events.size() == 2 + kRounds); // C + 20 条消息 + X
            CHECK(log.events.front() == "C");
            CHECK(log.events.back() == "X:PeerClosed:0"); // 恰一个 OnClosed，且在最后一个 OnMessage 之后
        }
    }
}

// 回调内自己 Close：OnClosed 恰一次、在最后一个 OnMessage 之后；同一个 TCP 段里紧跟着的下一帧不会再被分发。
void TestSelfCloseInHandler(const Target& t)
{
    const FdLeakCheck leak;
    RecordingHandler h;
    h.close_trigger = "bye";
    Server s(h, t.make());
    Client c(s.ep());
    std::vector<std::byte> both = Client::Frame("bye");
    const auto after = Client::Frame("after");
    both.insert(both.end(), after.begin(), after.end());
    c.SendAll(both); // 一次发出，服务端一次 readv 就收到两帧

    CHECK(c.RecvFrame() == "bye"); // Close 之前已经写出的回显照样送达
    CHECK(c.WaitClosed());
    const auto logs = h.WaitFinished(1);
    CHECK((logs[0].events == Ev({"C", "M:3", "X:LocalClose:0"}))); // 没有 "after"，也没有第二个 X
    CHECK(logs[0].send_after_close_closed);                        // Close 之后 Send 返回 Closed
    CHECK(!logs[0].connected_after_close);
    std::this_thread::sleep_for(50ms);
    CHECK(h.FinishedCountNow() == 1); // 之后不会再有回调
}

void TestCloseInOnConnected(const Target& t)
{
    const FdLeakCheck leak;
    RecordingHandler h;
    h.close_on_connect = true;
    Server s(h, t.make());
    Client c(s.ep());
    CHECK(c.WaitClosed());
    const auto logs = h.WaitFinished(1);
    CHECK((logs[0].events == Ev({"C", "X:LocalClose:0"})));
}

void TestProtocolError(const Target& t)
{
    const FdLeakCheck leak;
    RecordingHandler h;
    Server s(h, t.make(), 1024); // 上限 1024
    Client c(s.ep());
    c.SendFrame("ok");
    CHECK(c.RecvFrame() == "ok");
    const std::vector<std::byte> bad = {std::byte{0}, std::byte{0}, std::byte{0x13}, std::byte{0x88}}; // 声明 5000
    c.SendAll(bad);
    CHECK(c.WaitClosed());
    const auto logs = h.WaitFinished(1);
    CHECK((logs[0].events == Ev({"C", "M:2", "X:ProtocolError:0"})));
}

// Send 的 payload 超过 codec 上限：本端先关，不让对端带着错位的流继续跑。
void TestOversizeSend(const Target& t)
{
    const FdLeakCheck leak;
    RecordingHandler h;
    h.oversize_trigger = "big";
    Server s(h, t.make(), 1024);
    Client c(s.ep());
    c.SendFrame("big");
    CHECK(c.WaitClosed());
    const auto logs = h.WaitFinished(1);
    CHECK((logs[0].events == Ev({"C", "M:3", "X:ProtocolError:0"})));
}

// 写积压：一次回调里连发 10MB，远超内核缓冲，必须排队、靠 EPOLLOUT 排空，且保序、内容无损。
void TestWriteBacklogKeepsOrder(const Target& t)
{
    const FdLeakCheck leak;
    RecordingHandler h;
    h.flood_trigger = "flood";
    Server s(h, t.make());
    Client c(s.ep());
    c.SendFrame("flood");
    c.SendFrame("tail"); // 积压期间到达的回显必须排在所有积压帧之后，不能插队
    for (int i = 0; i < RecordingHandler::kFloodFrames; ++i)
    {
        const std::string f = c.RecvFrame();
        CHECK(f.size() == RecordingHandler::kFloodFrameSize);
        CHECK(f.front() == 'a' + i % 26 && f.back() == 'a' + i % 26); // 第 i 帧就是第 i 帧
    }
    CHECK(c.RecvFrame() == "tail");
}

// 视图版 Send：返回后原内存被立刻毁掉，客户端收到的仍是原内容（没写出去的部分已被库拷走）。
// 3MB 的帧在内核缓冲里通常写不完，覆盖"直接写了一部分、剩下的拷贝入队"的路径。
void TestViewSendCopiesWhatItCannotWrite(const Target& t)
{
    const FdLeakCheck leak;
    RecordingHandler h;
    h.view_big_trigger = "viewbig";
    Server s(h, t.make());
    Client c(s.ep());
    c.SendFrame("viewbig");
    const std::string got = c.RecvFrame();
    CHECK(got.size() == RecordingHandler::kViewBigSize);
    for (std::size_t i = 0; i < got.size(); ++i)
        CHECK(got[i] == RecordingHandler::ViewBigByte(i));
    c.SendFrame("after"); // 之后连接照常
    CHECK(c.RecvFrame() == "after");
}

// 已经有积压时，视图版 Send 不能直接写（会插队）：必须拷贝后排在积压之后，且内容无损、顺序不乱。
void TestViewSendBehindBacklogKeepsOrder(const Target& t)
{
    const FdLeakCheck leak;
    RecordingHandler h;
    h.view_backlog_trigger = "viewbacklog";
    Server s(h, t.make());
    Client c(s.ep());
    c.SendFrame("viewbacklog");
    for (int i = 0; i < RecordingHandler::kFloodFrames; ++i)
    {
        const std::string f = c.RecvFrame();
        CHECK(f.size() == RecordingHandler::kFloodFrameSize);
        CHECK(f.front() == 'a' + i % 26 && f.back() == 'a' + i % 26);
    }
    const std::string big = c.RecvFrame(); // 排在所有积压帧之后
    CHECK(big.size() == RecordingHandler::kViewBigSize);
    for (std::size_t i = 0; i < big.size(); ++i)
        CHECK(big[i] == RecordingHandler::ViewBigByte(i));
}

// 默认回显走视图版；这里单独把"先拷成 SharedBuffer 再发"那条路也过一遍，两个重载都有覆盖。
void TestEchoViaSharedBuffer(const Target& t)
{
    const FdLeakCheck leak;
    RecordingHandler h;
    h.echo_copy = true;
    Server s(h, t.make());
    Client c(s.ep());
    for (const std::string& p : {std::string("hello"), std::string(), std::string(1000, 'x'), std::string(70000, 'y')})
    {
        c.SendFrame(p);
        CHECK(c.RecvFrame() == p);
    }
}

void TestPeerInfo(const Target& t)
{
    const FdLeakCheck leak;
    RecordingHandler h;
    Server s(h, t.make());
    Client c(s.ep());
    CHECK(h.WaitConnected(1));
    c.Close();
    const auto logs = h.WaitFinished(1);
    if (t.is_tcp)
    {
        CHECK(logs[0].peer_endpoint.has_value() && logs[0].peer_endpoint->address() == "127.0.0.1");
        CHECK(!logs[0].peer_credentials.has_value()); // 只有 UDS 有 SO_PEERCRED
    }
    else
    {
        CHECK(!logs[0].peer_endpoint.has_value()); // 客户端没有绑定路径：未命名
        CHECK(logs[0].peer_credentials.has_value());
        CHECK(logs[0].peer_credentials->pid == ::getpid());
        CHECK(logs[0].peer_credentials->uid == ::getuid());
    }
}

// 对端 RST：不是正常关闭，原因是 IoError(ECONNRESET)。只有 TCP 有 RST。
void TestPeerReset()
{
    const FdLeakCheck leak;
    RecordingHandler h;
    Server s(h, *Endpoint::Tcp("127.0.0.1", 0));
    Client c(s.ep());
    CHECK(h.WaitConnected(1));
    c.ArmReset();
    c.Close();
    const auto logs = h.WaitFinished(1);
    CHECK((logs[0].events == Ev({"C", "X:IoError:104"}))); // ECONNRESET
}

// fd 耗尽：accept 遇到 EMFILE 时 loop 不能空转，待接受的连接被接出来关掉（对端得到 EOF 而不是挂起），
// 限额恢复后新连接照常可用。只测 TCP（效果与传输无关）。
void TestAcceptWhenFdsExhausted()
{
    const FdLeakCheck leak;
    RecordingHandler h;
    Server s(h, *Endpoint::Tcp("127.0.0.1", 0));

    // 客户端 socket 先建好：限额降下来之后本进程就不能再开新 fd 了。
    constexpr int kClients = 3;
    std::vector<UniqueFd> clients(kClients);
    for (auto& fd : clients)
        CHECK(sock::CreateStreamSocket(s.ep(), fd) == 0);

    rlimit old{};
    CHECK(::getrlimit(RLIMIT_NOFILE, &old) == 0);
    const int lowest_free = ::dup(0); // 当前最小的空闲 fd 号
    CHECK(lowest_free >= 0);
    ::close(lowest_free);
    rlimit low = old;
    low.rlim_cur = static_cast<rlim_t>(lowest_free); // 之后分配的 fd 号 ≥ lowest_free，全部 EMFILE
    CHECK(::setrlimit(RLIMIT_NOFILE, &low) == 0);

    for (auto& fd : clients)
    {
        const int rc = sock::Connect(fd.get(), s.ep());
        CHECK(rc == 0 || rc == EINPROGRESS);
    }
    const auto deadline = std::chrono::steady_clock::now() + kWait;
    while (s.srv.acceptor().rejected_fd_exhausted() < kClients)
    {
        CHECK(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(500us);
    }
    for (auto& fd : clients) // 每条都被接出来关掉了：读到 EOF（或 RST），而不是一直挂着
    {
        pollfd p{fd.get(), POLLIN, 0};
        CHECK(::poll(&p, 1, 5000) > 0);
        char b;
        const ssize_t n = ::recv(fd.get(), &b, 1, 0);
        CHECK(n == 0 || (n < 0 && errno == ECONNRESET));
    }

    // 不空转：处理完之后 loop 应该安静下来。空转的话这 300ms 里会跑几十万轮。
    const auto it0 = s.lt.loop.Iterations();
    std::this_thread::sleep_for(300ms);
    CHECK(s.lt.loop.Iterations() - it0 < 100);

    CHECK(::setrlimit(RLIMIT_NOFILE, &old) == 0);
    clients.clear();

    // 恢复后新连接照常
    Client ok(s.ep());
    ok.SendFrame("alive");
    CHECK(ok.RecvFrame() == "alive");
    CHECK(h.WaitConnected(1)); // 被拒绝的那 3 条没有任何回调：只有这一条回调过 OnConnected
}

} // namespace

int main()
{
    const std::string tmp = MakeTmpDir();
    std::atomic<int> seq{0};
    const std::vector<Target> targets = {
        {"tcp", [] { return *Endpoint::Tcp("127.0.0.1", 0); }, true},
        {"unix", [&] { return *Endpoint::Unix(tmp + "/s" + std::to_string(seq.fetch_add(1)) + ".sock"); }, false},
        {"unix-abstract",
         [&]
         {
             return *Endpoint::UnixAbstract("breeze-echo-" + std::to_string(::getpid()) + "-" +
                                            std::to_string(seq.fetch_add(1)));
         },
         false},
    };

    for (const Target& t : targets)
    {
        std::printf("echo_test [%s]\n", t.label);
        TestEcho(t);
        TestFrameSplitAcrossReads(t);
        TestLargeFrame(t);
        TestPipelined(t);
        TestManyConnections(t);
        TestSelfCloseInHandler(t);
        TestCloseInOnConnected(t);
        TestProtocolError(t);
        TestOversizeSend(t);
        TestWriteBacklogKeepsOrder(t);
        TestViewSendCopiesWhatItCannotWrite(t);
        TestViewSendBehindBacklogKeepsOrder(t);
        TestEchoViaSharedBuffer(t);
        TestPeerInfo(t);
    }
    TestPeerReset();
    TestAcceptWhenFdsExhausted();

    // 清理 UDS 临时目录
    if (DIR* d = ::opendir(tmp.c_str()))
    {
        while (const dirent* e = ::readdir(d))
        {
            if (e->d_name[0] != '.')
                ::unlink((tmp + "/" + e->d_name).c_str());
        }
        ::closedir(d);
    }
    ::rmdir(tmp.c_str());

    std::puts("echo_test: ok");
    return 0;
}
