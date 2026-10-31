#include "socket_config.h"

#include "logger.h"
#include "logger_macros.h"
#include "tcp_socket.h"
#include "uds_socket.h"

namespace breeze
{

void ConfigureSocket(int fd, Endpoint::Kind kind, const TcpOptions& tcp, PeerInfo& info)
{
    if (kind == Endpoint::Kind::Tcp)
    {
        if (const int e = tcp::SetNoDelay(fd); e != 0)
            LOG_WARN("设置 TCP_NODELAY 失败 fd={} errno={}", fd, e);
        if (tcp.keepalive_idle.count() > 0)
        {
            const tcp::KeepAlive ka{static_cast<int>(tcp.keepalive_idle.count()),
                                    static_cast<int>(tcp.keepalive_interval.count()),
                                    tcp.keepalive_count,
                                    static_cast<int>(tcp.tcp_user_timeout.count())};
            if (const int e = tcp::SetKeepAlive(fd, ka); e != 0)
                LOG_WARN("设置 keepalive 失败 fd={} errno={}", fd, e);
        }
        else if (tcp.tcp_user_timeout.count() > 0)
        {
            if (const int e = tcp::SetUserTimeout(fd, static_cast<int>(tcp.tcp_user_timeout.count())); e != 0)
                LOG_WARN("设置 TCP_USER_TIMEOUT 失败 fd={} errno={}", fd, e);
        }
        return;
    }
    if (kind == Endpoint::Kind::Udp)
        return; // 数据报没有"连接"：这个函数只给字节流用，UDP 端点不该走到这里
    PeerCredentials cred;
    if (uds::GetPeerCredentials(fd, cred) == 0)
        info.credentials = cred;
}

} // namespace breeze
