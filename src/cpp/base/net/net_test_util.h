// 网络库测试共用的小工具（只给 *_test.cpp 用，不进库）：CHECK、带超时的等待、fd / 线程泄漏检查、阻塞风格的测试客户端。
// 无测试框架依赖：失败即 abort，并发用例带超时，卡死也会 abort 而不是挂住 ctest。

#pragma once

#include <dirent.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <unordered_map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "buffer.h"
#include "connection.h"
#include "connection_handler.h"
#include "endpoint.h"
#include "event_loop.h"
#include "length_prefix_codec.h"
#include "shared_buffer.h"
#include "socket.h"

#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                              \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (0)

namespace breeze::testing
{

using namespace std::chrono_literals;

constexpr auto kWait = 10s;

template <typename Pred>
bool WaitUntil(Pred pred, std::chrono::milliseconds timeout = kWait)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred())
    {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        std::this_thread::sleep_for(200us);
    }
    return true;
}

inline SharedBuffer Buf(std::string_view s)
{
    return SharedBuffer::CopyFrom({reinterpret_cast<const std::byte*>(s.data()), s.size()});
}

inline std::string Str(std::span<const std::byte> b) { return {reinterpret_cast<const char*>(b.data()), b.size()}; }

// 回显：EventRecorder::on_message 的常用实现，用视图版 Send 直接从读缓冲写回。
inline void Echo(Connection& c, std::string_view sv)
{
    (void)c.Send(std::as_bytes(std::span(sv)));
}

// 临时目录，放 UDS 的 socket 文件；析构时清掉里面的文件与目录本身。
class TempDir
{
public:
    TempDir()
    {
        char tmpl[] = "/tmp/breeze-net-XXXXXX";
        const char* p = ::mkdtemp(tmpl);
        CHECK(p != nullptr);
        path_ = p;
    }
    ~TempDir()
    {
        if (DIR* d = ::opendir(path_.c_str()))
        {
            while (const dirent* e = ::readdir(d))
            {
                if (e->d_name[0] != '.')
                    ::unlink((path_ + "/" + e->d_name).c_str());
            }
            ::closedir(d);
        }
        ::rmdir(path_.c_str());
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::string& path() const { return path_; }
    Endpoint Unix(const std::string& name) const
    {
        const auto ep = Endpoint::Unix(path_ + "/" + name);
        CHECK(ep.has_value());
        return *ep;
    }

private:
    std::string path_;
};

// 在 loop 线程上执行 fn 并等它跑完（loop 已经在运行时，Acceptor::Listen 之类只能在 loop 线程调用）。
inline void RunOnLoop(EventLoop& loop, std::function<void()> fn)
{
    std::atomic<bool> done{false};
    loop.Post(
        [&]
        {
            fn();
            done.store(true);
        });
    CHECK(WaitUntil([&] { return done.load(); }));
}

inline int CountEntries(const char* dir)
{
    int n = 0;
    if (DIR* d = ::opendir(dir))
    {
        while (::readdir(d) != nullptr)
            ++n;
        ::closedir(d);
    }
    return n;
}

// 作用域前后打开的 fd 数与线程数必须相同：没有 fd / 线程泄漏。线程退出到内核回收有一点延迟，所以析构时等一等。
struct LeakCheck
{
    int fds = CountEntries("/proc/self/fd");
    int threads = CountEntries("/proc/self/task");
    ~LeakCheck()
    {
        const bool ok = WaitUntil([&] {
            return CountEntries("/proc/self/fd") == fds && CountEntries("/proc/self/task") == threads;
        });
        if (!ok)
        {
            std::fprintf(stderr,
                         "泄漏：fd %d → %d，线程 %d → %d\n",
                         fds,
                         CountEntries("/proc/self/fd"),
                         threads,
                         CountEntries("/proc/self/task"));
            std::abort();
        }
    }
};

// 线程安全地记录连接事件。可选的钩子在 IO 线程上、记录之后调用（要回显、要在回调里关连接 / 暂停读时用）。
// CheckInvariants 验证设计 §7 第 2 条：每条连接 OnConnected 先于一切，恰一个 OnClosed 且在最后一个 OnMessage 之后。
class EventRecorder final : public ConnectionHandler
{
public:
    struct Closed
    {
        ConnectionHandle handle;
        CloseReason reason;
        int err;
    };
    struct Message
    {
        ConnectionHandle handle;
        std::string payload;
    };

    std::function<void(Connection&)> on_connected;
    std::function<void(Connection&, std::string_view)> on_message;
    std::function<void(Connection&, CloseReason)> on_closed;

    void OnConnected(Connection& c) noexcept override
    {
        {
            const std::lock_guard lk(mu_);
            connected_.push_back(c.handle());
            seq_[c.handle()].push_back('C');
        }
        if (on_connected)
            on_connected(c);
    }
    void OnMessage(Connection& c, std::span<const std::byte> payload) noexcept override
    {
        const std::string s = Str(payload);
        {
            const std::lock_guard lk(mu_);
            messages_.push_back({c.handle(), s});
            seq_[c.handle()].push_back('M');
        }
        if (on_message)
            on_message(c, s);
    }
    void OnClosed(Connection& c, CloseReason r, int err) noexcept override
    {
        {
            const std::lock_guard lk(mu_);
            closed_.push_back({c.handle(), r, err});
            seq_[c.handle()].push_back('X');
        }
        if (on_closed)
            on_closed(c, r);
    }

    std::size_t connected_count() const
    {
        const std::lock_guard lk(mu_);
        return connected_.size();
    }
    std::size_t message_count() const
    {
        const std::lock_guard lk(mu_);
        return messages_.size();
    }
    std::size_t closed_count() const
    {
        const std::lock_guard lk(mu_);
        return closed_.size();
    }
    std::vector<std::string> payloads() const
    {
        const std::lock_guard lk(mu_);
        std::vector<std::string> out;
        for (const auto& m : messages_)
            out.push_back(m.payload);
        return out;
    }

    bool WasClosed(ConnectionHandle h) const
    {
        const std::lock_guard lk(mu_);
        for (const auto& c : closed_)
            if (c.handle == h)
                return true;
        return false;
    }

    ConnectionHandle WaitConnected(std::size_t i, std::chrono::milliseconds timeout = kWait) const
    {
        CHECK(WaitUntil([&] { return connected_count() > i; }, timeout));
        const std::lock_guard lk(mu_);
        return connected_[i];
    }
    Closed WaitClosed(std::size_t i, std::chrono::milliseconds timeout = kWait) const
    {
        CHECK(WaitUntil([&] { return closed_count() > i; }, timeout));
        const std::lock_guard lk(mu_);
        return closed_[i];
    }
    bool WaitMessages(std::size_t n, std::chrono::milliseconds timeout = kWait) const
    {
        return WaitUntil([&] { return message_count() >= n; }, timeout);
    }

    // 每条连接的事件序列必须匹配 C M* X?（X 至多一个，且最后）。
    void CheckInvariants() const
    {
        const std::lock_guard lk(mu_);
        for (const auto& [h, seq] : seq_)
        {
            CHECK(!seq.empty() && seq.front() == 'C');
            std::size_t closes = 0;
            for (std::size_t i = 0; i < seq.size(); ++i)
            {
                if (seq[i] == 'X')
                {
                    ++closes;
                    CHECK(i + 1 == seq.size()); // OnClosed 之后不再有该连接的任何回调
                }
            }
            CHECK(closes <= 1);
        }
    }

private:
    mutable std::mutex mu_;
    std::vector<ConnectionHandle> connected_;
    std::vector<Message> messages_;
    std::vector<Closed> closed_;
    std::unordered_map<ConnectionHandle, std::string> seq_; // 每条连接的事件序列：C / M / X
};

// 阻塞风格的测试客户端（LengthPrefixCodec 分帧），所有等待都带超时。
class TestClient
{
public:
    explicit TestClient(const Endpoint& ep)
    {
        CHECK(sock::Create(ep, fd_) == 0);
        const int rc = sock::Connect(fd_.get(), ep);
        CHECK(rc == 0 || rc == EINPROGRESS);
        CHECK(Wait(POLLOUT, kWait));
        CHECK(sock::SoError(fd_.get()) == 0);
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
    bool WaitClosed(std::chrono::milliseconds timeout = kWait)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        char junk[64 * 1024];
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

    bool Wait(short events, std::chrono::milliseconds timeout)
    {
        pollfd p{fd_.get(), events, 0};
        return ::poll(&p, 1, static_cast<int>(timeout.count())) > 0;
    }

private:
    Fd fd_;
    Buffer buf_;
    LengthPrefixCodec codec_{16 * 1024 * 1024};
};

} // namespace breeze::testing
