// 连接建立时按传输类型分支设置 socket 选项：TCP 设 NODELAY / keepalive（tcp/），UDS 取 SO_PEERCRED（uds/）。
// 差异收在 tcp/ 与 uds/，Connection 以上不出现 if tcp（设计 docs/网络库设计.md §4.2）。
// 服务端 accept 与客户端 connect 成功都走它，所以单独放一个文件。

#pragma once

#include "endpoint.h"
#include "peer_info.h"
#include "tcp_options.h"

namespace breeze
{

// 选项设置失败只记告警，不影响连接（keepalive 之类是尽力而为）。UDS 取到的对端身份写进 info.credentials。
void ConfigureSocket(int fd, Endpoint::Kind kind, const TcpOptions& tcp, PeerInfo& info);

} // namespace breeze
