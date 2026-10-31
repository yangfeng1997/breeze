// UDS 专有的 socket 处理：socket 文件的残留清理与删除、SO_PEERCRED 取对端身份。
// 字节流共用的部分见 internal/stream/socket.h。
//
// 约定：返回 int 的函数返回 errno（0 = 成功），不抛异常。

#pragma once

#include "endpoint.h"
#include "peer_info.h"

namespace breeze
{

namespace uds
{

// bind 之前对路径上的 UDS 文件做探测：连得上（有人在监听）→ EADDRINUSE；连不上（残留）→ unlink 后返回 0；
// 路径上不是 socket 的文件不动，直接 EADDRINUSE。只对 Kind::Unix 有意义（抽象名没有文件）。
int ClearStaleSocket(const Endpoint& ep);

// 停止监听时删掉 socket 文件。只对 Kind::Unix 有意义；文件已经不在不算错。
void RemoveSocketFile(const Endpoint& ep);

// SO_PEERCRED：对端进程的 uid / gid / pid。
int GetPeerCredentials(int fd, PeerCredentials& out);

} // namespace uds

} // namespace breeze
