#include "base/net/stream/internal/fd_passing.h"

#include <sys/socket.h>

#include <array>
#include <cerrno>
#include <cstring>

namespace breeze::sock
{

namespace
{

// 控制缓冲：按 kMaxPassedFds 个 int 算，保证对齐。
constexpr std::size_t kControlSpace = CMSG_SPACE(kMaxPassedFds * sizeof(int));

union ControlBuffer
{
    char buf[kControlSpace];
    cmsghdr align;
};

} // namespace

int SendWithFds(int sock, std::span<const std::byte> data, std::span<const int> fds)
{
    if (fds.size() > kMaxPassedFds || data.empty())
        return EINVAL;

    iovec iov{const_cast<std::byte*>(data.data()), data.size()};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    ControlBuffer control;
    if (!fds.empty())
    {
        std::memset(&control, 0, sizeof(control));
        msg.msg_control = control.buf;
        msg.msg_controllen = CMSG_SPACE(fds.size() * sizeof(int));
        cmsghdr* c = CMSG_FIRSTHDR(&msg);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(fds.size() * sizeof(int));
        std::memcpy(CMSG_DATA(c), fds.data(), fds.size() * sizeof(int));
    }

    ssize_t n;
    do
    {
        n = ::sendmsg(sock, &msg, MSG_NOSIGNAL);
    } while (n < 0 && errno == EINTR);
    if (n < 0)
        return errno;

    // 没发完：fd 已经随第一段送出，剩下的用普通 send 补；补不下就放弃，由调用方关连接。
    std::size_t sent = static_cast<std::size_t>(n);
    while (sent < data.size())
    {
        const ssize_t m = ::send(sock, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (m < 0)
        {
            if (errno == EINTR)
                continue;
            return errno;
        }
        sent += static_cast<std::size_t>(m);
    }
    return 0;
}

int RecvWithFds(int sock, std::span<std::byte> buf, std::size_t& n, std::span<UniqueFd> fds_out, std::size_t& nfds)
{
    n = 0;
    nfds = 0;
    if (fds_out.size() > kMaxPassedFds || buf.empty())
        return EINVAL;

    iovec iov{buf.data(), buf.size()};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    ControlBuffer control;
    std::memset(&control, 0, sizeof(control));
    msg.msg_control = control.buf;
    // 只给调用方要的槽位那么大：对端塞多了，多出来的内核替我们关掉并置 MSG_CTRUNC。
    msg.msg_controllen = fds_out.empty() ? 0 : CMSG_SPACE(fds_out.size() * sizeof(int));

    ssize_t got;
    do
    {
        got = ::recvmsg(sock, &msg, MSG_CMSG_CLOEXEC);
    } while (got < 0 && errno == EINTR);
    if (got < 0)
        return errno;

    // 先把收到的 fd 全部接管（不管后面判成什么错，都不能漏关）。
    std::size_t received = 0;
    for (cmsghdr* c = (msg.msg_controllen > 0 ? CMSG_FIRSTHDR(&msg) : nullptr); c != nullptr; c = CMSG_NXTHDR(&msg, c))
    {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS)
            continue;
        const std::size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        for (std::size_t i = 0; i < count; ++i)
        {
            int fd;
            std::memcpy(&fd, CMSG_DATA(c) + i * sizeof(int), sizeof(int));
            // 控制缓冲就是按槽位数给的，收到的不会比槽位多；先交给 UniqueFd 接管，万一多了也会随它析构关闭。
            UniqueFd owned(fd);
            if (received < fds_out.size())
                fds_out[received++] = std::move(owned);
        }
    }
    if ((msg.msg_flags & MSG_CTRUNC) != 0)
    {
        for (std::size_t i = 0; i < received; ++i)
            fds_out[i].reset();
        return EMSGSIZE;
    }
    n = static_cast<std::size_t>(got);
    nfds = received;
    return 0;
}

} // namespace breeze::sock
