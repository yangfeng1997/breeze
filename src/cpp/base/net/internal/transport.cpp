#include "transport.h"

#include "logger.h"
#include "logger_macros.h"
#include "socket.h"

namespace breeze
{

void SetupTransport(int fd, Endpoint::Kind kind, const ConnectionOptions& options, PeerInfo& info)
{
    if (kind == Endpoint::Kind::Tcp)
    {
        if (const int e = sock::SetNoDelay(fd); e != 0)
            LOG_WARN("设置 TCP_NODELAY 失败 fd={} errno={}", fd, e);
        if (options.keepalive_idle.count() > 0)
        {
            const KeepAlive ka{static_cast<int>(options.keepalive_idle.count()),
                               static_cast<int>(options.keepalive_interval.count()),
                               options.keepalive_count,
                               static_cast<int>(options.tcp_user_timeout.count())};
            if (const int e = sock::SetKeepAlive(fd, ka); e != 0)
                LOG_WARN("设置 keepalive 失败 fd={} errno={}", fd, e);
        }
        else if (options.tcp_user_timeout.count() > 0)
        {
            if (const int e = sock::SetUserTimeout(fd, static_cast<int>(options.tcp_user_timeout.count())); e != 0)
                LOG_WARN("设置 TCP_USER_TIMEOUT 失败 fd={} errno={}", fd, e);
        }
        return;
    }
    PeerCredentials cred;
    if (sock::GetPeerCredentials(fd, cred) == 0)
        info.credentials = cred;
}

} // namespace breeze
