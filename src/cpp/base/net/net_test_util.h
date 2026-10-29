// 网络库测试共用的小工具（只给 *_test.cpp 用，不进库）：CHECK、带超时的等待、fd / 线程泄漏检查、阻塞风格的测试客户端。
// 无测试框架依赖：失败即 abort，并发用例带超时，卡死也会 abort 而不是挂住 ctest。

#pragma once

#include <dirent.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "buffer.h"
#include "endpoint.h"
#include "event_loop.h"
#include "length_prefix_codec.h"
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
