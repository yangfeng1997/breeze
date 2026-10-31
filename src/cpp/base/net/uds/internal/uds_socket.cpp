#include "uds_socket.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>

#include "socket_address.h"
#include "unique_fd.h"

namespace breeze
{

namespace uds
{

// 探测路径上的 UDS 文件是否有人在监听；没人监听（残留）就删掉，让随后的 bind 能成功。
int ClearStaleSocket(const Endpoint& ep)
{
    struct stat st{};
    if (::lstat(ep.address().c_str(), &st) != 0)
        return errno == ENOENT ? 0 : errno;
    if (!S_ISSOCK(st.st_mode))
        return EADDRINUSE; // 不是 socket 的文件不动，更不 unlink

    UniqueFd probe;
    if (const int err = sock::CreateSocket(ep, SOCK_STREAM, probe); err != 0)
        return err;
    sockaddr_storage ss;
    const socklen_t len = sock::ToSockaddr(ep, ss);
    if (::connect(probe.get(), reinterpret_cast<const sockaddr*>(&ss), len) == 0)
        return EADDRINUSE; // 连上了：有人在监听
    switch (errno)
    {
        case EAGAIN: // 非阻塞 UDS connect 在对方 backlog 已满时返回它：同样说明有人在监听
        case EINPROGRESS:
            return EADDRINUSE;
        case ECONNREFUSED: // 文件还在但没人监听：上次进程没来得及清理的残留
            return (::unlink(ep.address().c_str()) == 0 || errno == ENOENT) ? 0 : errno;
        case ENOENT: // 探测期间被别人删掉了
            return 0;
        default:
            return errno;
    }
}

void RemoveSocketFile(const Endpoint& ep) { ::unlink(ep.address().c_str()); }

int GetPeerCredentials(int fd, PeerCredentials& out)
{
    ucred cred{};
    socklen_t len = sizeof(cred);
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0)
        return errno;
    out = PeerCredentials{cred.uid, cred.gid, cred.pid};
    return 0;
}

} // namespace uds

} // namespace breeze
