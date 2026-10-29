// socket 层的薄封装：fd RAII 与建立连接时的各种选项。设计见 docs/网络库设计.md §4.2 / §4.7。
//
// 约定：返回 int 的函数返回 errno（0 = 成功），不抛异常；失败时输出参数保持不变。
// Connection 以上不出现 if tcp：TCP / UDS 的差异都收在这里（建立时设选项、UDS 取 SO_PEERCRED）。

#pragma once

#include <optional>

#include "endpoint.h"
#include "peer_info.h"

namespace breeze
{

// 拥有一个 fd；析构时关闭。
class Fd
{
public:
    Fd() noexcept = default;
    explicit Fd(int fd) noexcept
        : fd_(fd)
    {
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& o) noexcept
        : fd_(o.release())
    {
    }
    Fd& operator=(Fd&& o) noexcept
    {
        if (this != &o)
            reset(o.release());
        return *this;
    }
    ~Fd() { reset(); }

    int get() const noexcept { return fd_; }
    bool valid() const noexcept { return fd_ >= 0; }
    int release() noexcept
    {
        const int f = fd_;
        fd_ = -1;
        return f;
    }
    void reset(int fd = -1) noexcept;

private:
    int fd_ = -1;
};

// TCP keepalive 参数。只开 SO_KEEPALIVE 而不设参数时内核默认 7200s，等于没开，所以三个参数一起设。
struct KeepAlive
{
    int idle_s = 60;
    int interval_s = 10;
    int count = 3;
    int user_timeout_ms = 0; // TCP_USER_TIMEOUT：已发出的数据多久得不到确认就断开；0 = 不设
};

namespace sock
{

// 新建流式 socket（非阻塞、CLOEXEC），协议族由 endpoint 决定。
int Create(const Endpoint& ep, Fd& out);

int SetNoDelay(int fd);
int SetReuseAddr(int fd);
int SetKeepAlive(int fd, const KeepAlive& ka); // 仅 TCP

// UDS 路径会先探测残留的 socket 文件：连得上（有人在监听）→ EADDRINUSE；连不上（残留）→ unlink 后再 bind；
// 路径上不是 socket 的文件不动，直接 EADDRINUSE。
int Bind(int fd, const Endpoint& ep);
int Listen(int fd, int backlog);

// 非阻塞 connect：0 = 已连上；EINPROGRESS = 进行中，等 EPOLLOUT 后用 SoError 判定；其它为失败。
int Connect(int fd, const Endpoint& ep);

// accept4（非阻塞、CLOEXEC）。EAGAIN = 没有更多待接受的连接。peer 为对端地址，未命名的 UDS 对端为空。
int Accept(int listen_fd, Fd& out, std::optional<Endpoint>& peer);

// 读 SO_ERROR 并清除。返回该 errno（0 = 没有错误）；getsockopt 自己失败时返回它的 errno。
int SoError(int fd);

int LocalEndpoint(int fd, std::optional<Endpoint>& out);
int PeerEndpoint(int fd, std::optional<Endpoint>& out);

// SO_PEERCRED，仅 UDS。
int GetPeerCredentials(int fd, PeerCredentials& out);

// 本地与对端的地址、端口完全相同：loopback 重连时 TCP 同时打开造成的"连到了自己"。
bool IsSelfConnect(int fd);

} // namespace sock

} // namespace breeze
