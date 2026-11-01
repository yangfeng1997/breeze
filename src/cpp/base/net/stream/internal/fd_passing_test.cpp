// UDS 传 fd 的封装：传递、CLOEXEC、对端塞太多 fd 时不泄漏、EOF / EAGAIN / 参数错误。
// 无测试框架依赖：失败即 abort。

#include "base/net/stream/internal/fd_passing.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "base/net/socket/unique_fd.h"
#include "base/net/testing/net_test_util.h"

using namespace breeze;
using namespace breeze::testing;

namespace
{

struct Pair
{
    UniqueFd a, b;
};

Pair MakePair()
{
    int sv[2];
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sv) == 0);
    return {UniqueFd(sv[0]), UniqueFd(sv[1])};
}

std::array<UniqueFd, 2> MakePipe()
{
    int p[2];
    CHECK(::pipe2(p, O_CLOEXEC) == 0);
    return {UniqueFd(p[0]), UniqueFd(p[1])};
}

std::span<const std::byte> Bytes(std::string_view s) { return std::as_bytes(std::span(s)); }

void TestPassFds()
{
    Pair p = MakePair();
    auto pipe_a = MakePipe();
    auto pipe_b = MakePipe();
    const std::array<int, 2> fds{pipe_a[1].get(), pipe_b[0].get()}; // 一个写端、一个读端

    CHECK(sock::SendWithFds(p.a.get(), Bytes("hello"), fds) == 0);

    std::array<std::byte, 16> buf{};
    std::array<UniqueFd, 4> got;
    std::size_t n = 0, nf = 0;
    CHECK(sock::RecvWithFds(p.b.get(), buf, n, got, nf) == 0);
    CHECK(n == 5 && std::memcmp(buf.data(), "hello", 5) == 0);
    CHECK(nf == 2 && got[0].valid() && got[1].valid());
    CHECK(!got[2].valid() && !got[3].valid());

    // 收到的 fd 带 CLOEXEC（MSG_CMSG_CLOEXEC）：不会泄漏进 exec 出来的子进程。
    CHECK((::fcntl(got[0].get(), F_GETFD) & FD_CLOEXEC) != 0);
    CHECK((::fcntl(got[1].get(), F_GETFD) & FD_CLOEXEC) != 0);

    // 收到的是真的同一个对象：经收到的写端写、从原来的读端读。
    CHECK(::write(got[0].get(), "x", 1) == 1);
    char c = 0;
    CHECK(::read(pipe_a[0].get(), &c, 1) == 1 && c == 'x');
    // 反过来：往原来的写端写，从收到的读端读。
    CHECK(::write(pipe_b[1].get(), "y", 1) == 1);
    CHECK(::read(got[1].get(), &c, 1) == 1 && c == 'y');
}

void TestNoFds()
{
    Pair p = MakePair();
    CHECK(sock::SendWithFds(p.a.get(), Bytes("abc"), {}) == 0);
    std::array<std::byte, 8> buf{};
    std::array<UniqueFd, 3> got;
    std::size_t n = 99, nf = 99;
    CHECK(sock::RecvWithFds(p.b.get(), buf, n, got, nf) == 0);
    CHECK(n == 3 && nf == 0);
}

// 对端塞的 fd 比我们要的多：整条按协议错误处理，已收到的全部关闭，一个都不能漏进 fd 表。
void TestTooManyFdsDoNotLeak()
{
    Pair p = MakePair();
    std::vector<UniqueFd> keep; // 要传出去的 fd，由发送方（本测试）持有
    std::array<int, 5> raw{};
    for (int& r : raw)
    {
        auto pipe = MakePipe();
        keep.push_back(std::move(pipe[1]));
        r = keep.back().get();
    }
    CHECK(sock::SendWithFds(p.a.get(), Bytes("data"), raw) == 0);

    const int before = CountEntries("/proc/self/fd");
    std::array<std::byte, 8> buf{};
    std::array<UniqueFd, 2> got; // 只给 2 个槽位，对端塞了 5 个
    std::size_t n = 99, nf = 99;
    CHECK(sock::RecvWithFds(p.b.get(), buf, n, got, nf) == EMSGSIZE);
    CHECK(n == 0 && nf == 0);
    CHECK(!got[0].valid() && !got[1].valid()); // 已经收到的也关了
    CHECK(CountEntries("/proc/self/fd") == before);
}

// 一个槽位都不给（期望这条消息不带 fd）：对端硬塞的 fd 被内核丢弃，同样不进 fd 表。
void TestUnexpectedFdsAreDropped()
{
    Pair p = MakePair();
    auto pipe_a = MakePipe();
    const std::array<int, 1> fds{pipe_a[1].get()};
    CHECK(sock::SendWithFds(p.a.get(), Bytes("data"), fds) == 0);

    const int before = CountEntries("/proc/self/fd");
    std::array<std::byte, 8> buf{};
    std::size_t n = 99, nf = 99;
    CHECK(sock::RecvWithFds(p.b.get(), buf, n, {}, nf) == EMSGSIZE);
    CHECK(CountEntries("/proc/self/fd") == before);

    // 普通 recv（握手读 Hello 用的）：附带的 fd 同样被丢弃，数据照常读到，fd 表不变。
    CHECK(sock::SendWithFds(p.a.get(), Bytes("more"), fds) == 0);
    char raw[8];
    const int before2 = CountEntries("/proc/self/fd");
    CHECK(::recv(p.b.get(), raw, sizeof(raw), 0) > 0);
    CHECK(CountEntries("/proc/self/fd") == before2);
}

void TestEofAgainAndErrors()
{
    Pair p = MakePair();
    std::array<std::byte, 8> buf{};
    std::array<UniqueFd, 2> got;
    std::size_t n = 99, nf = 99;

    CHECK(sock::RecvWithFds(p.b.get(), buf, n, got, nf) == EAGAIN); // 没有数据
    CHECK(n == 0 && nf == 0);

    p.a.reset();                                                                    // 对端关闭
    CHECK(sock::RecvWithFds(p.b.get(), buf, n, got, nf) == 0 && n == 0 && nf == 0); // EOF

    // 参数错误。
    Pair q = MakePair();
    std::array<int, sock::kMaxPassedFds + 1> too_many{};
    CHECK(sock::SendWithFds(q.a.get(), Bytes("x"), too_many) == EINVAL);
    CHECK(sock::SendWithFds(q.a.get(), {}, {}) == EINVAL);
    std::array<UniqueFd, sock::kMaxPassedFds + 1> too_many_slots;
    CHECK(sock::RecvWithFds(q.b.get(), buf, n, too_many_slots, nf) == EINVAL);
    CHECK(sock::RecvWithFds(q.b.get(), {}, n, got, nf) == EINVAL);

    // 对端已经关闭之后再发：返回 EPIPE（不是被 SIGPIPE 杀掉）。
    Pair r = MakePair();
    r.b.reset();
    CHECK(sock::SendWithFds(r.a.get(), Bytes("x"), {}) == EPIPE);
}

void TestLargerMessageSplitsAcrossReads()
{
    // 数据比一次读的缓冲大：fd 随第一段到，其余是普通数据，分几次读完。
    Pair p = MakePair();
    auto pipe_a = MakePipe();
    const std::array<int, 1> fds{pipe_a[1].get()};
    std::string msg(100, 'z');
    CHECK(sock::SendWithFds(p.a.get(), Bytes(msg), fds) == 0);

    std::array<std::byte, 30> buf{};
    std::array<UniqueFd, 2> got;
    std::size_t total = 0, total_fds = 0;
    while (total < msg.size())
    {
        std::size_t n = 0, nf = 0;
        CHECK(sock::RecvWithFds(p.b.get(), buf, n, got, nf) == 0);
        CHECK(n > 0);
        total += n;
        total_fds += nf;
    }
    CHECK(total == msg.size() && total_fds == 1);
}

void NoopHandler(int) {}

// 阻塞的 socketpair + 很小的发送缓冲 + 接收端晚点才读 + 不断给发送线程打信号：
// sendmsg 会因为信号只发出一部分，剩下的靠补发。补发被信号打断要接着发，不能丢数据、不能报错。
void TestPartialSendsSurviveSignals()
{
    struct sigaction sa{};
    sa.sa_handler = NoopHandler; // 不带 SA_RESTART：阻塞的 send 被打断时返回 EINTR / 已发出的字节数
    sigemptyset(&sa.sa_mask);
    CHECK(::sigaction(SIGUSR1, &sa, nullptr) == 0);

    int sv[2];
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0);
    UniqueFd tx(sv[0]), rx(sv[1]);
    const int small = 4096;
    CHECK(::setsockopt(tx.get(), SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)) == 0);

    std::string msg(1 << 20, '\0');
    for (std::size_t i = 0; i < msg.size(); ++i)
        msg[i] = static_cast<char>(i % 251);
    auto pipe_a = MakePipe();
    const std::array<int, 1> fds{pipe_a[1].get()};

    std::atomic<bool> done{false};
    const pthread_t sender = ::pthread_self();
    std::thread kicker(
        [&]
        {
            while (!done.load())
            {
                ::pthread_kill(sender, SIGUSR1);
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        });
    bool received_ok = false;
    std::size_t received_fds = 0;
    std::thread receiver(
        [&]
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50)); // 让发送方先填满缓冲、被信号打断
            std::string got;
            std::vector<std::byte> buf(65536);
            std::array<UniqueFd, 2> slots;
            while (got.size() < msg.size())
            {
                pollfd p{rx.get(), POLLIN, 0};
                if (::poll(&p, 1, 3000) <= 0)
                    return; // 超时：收不全，received_ok 保持 false
                std::size_t n = 0, nf = 0;
                if (sock::RecvWithFds(rx.get(), buf, n, slots, nf) != 0 || n == 0)
                    return;
                got.append(reinterpret_cast<const char*>(buf.data()), n);
                received_fds += nf;
            }
            received_ok = (got == msg);
        });

    const int rc = sock::SendWithFds(tx.get(), std::as_bytes(std::span(msg)), fds);
    done.store(true);
    kicker.join();
    receiver.join();
    CHECK(rc == 0);
    CHECK(received_ok);       // 一个字节都没丢、没错位
    CHECK(received_fds == 1); // fd 只随第一段到一次，没有因为重试而重复
}

// 补发时对端关了：返回 EPIPE，而不是被 SIGPIPE 杀掉（补发那一步也必须带 MSG_NOSIGNAL）。
void TestBrokenPipeDuringPartialSend()
{
    int sv[2];
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0);
    UniqueFd tx(sv[0]), rx(sv[1]);
    const int small = 4096;
    CHECK(::setsockopt(tx.get(), SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)) == 0);
    std::string msg(1 << 20, 'x');

    std::thread receiver(
        [&]
        {
            std::vector<std::byte> buf(65536);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            (void)::recv(rx.get(), buf.data(), buf.size(), 0); // 读一点（让发送方发出了一部分），然后关掉
            rx.reset();
        });
    const int rc = sock::SendWithFds(tx.get(), std::as_bytes(std::span(msg)), {});
    receiver.join();
    CHECK(rc == EPIPE || rc == ECONNRESET);
}

} // namespace

int main()
{
    LeakCheck leak;
    TestPassFds();
    TestNoFds();
    TestTooManyFdsDoNotLeak();
    TestUnexpectedFdsAreDropped();
    TestEofAgainAndErrors();
    TestLargerMessageSplitsAcrossReads();
    TestPartialSendsSurviveSignals();
    TestBrokenPipeDuringPartialSend();
    std::puts("fd_passing_test: OK");
    return 0;
}
