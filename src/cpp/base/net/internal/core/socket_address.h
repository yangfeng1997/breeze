// Endpoint 与 sockaddr 互转，以及按 Endpoint 的协议族新建 socket。字节流与以后的数据报都要用，所以放在 core。
//
// 约定：返回 int 的函数返回 errno（0 = 成功），不抛异常；失败时输出参数保持不变。

#pragma once

#include <sys/socket.h>

#include <optional>

#include "endpoint.h"
#include "unique_fd.h"

namespace breeze
{

namespace sock
{

// 返回 sockaddr 的有效长度。Endpoint 构造时已校验过，不会失败。
socklen_t ToSockaddr(const Endpoint& ep, sockaddr_storage& ss);

// 地址族不认识、未命名的 UDS 对端、或不满足 Endpoint 约束的地址，都得到 nullopt。
// IP 类地址得到 ip_kind 指定的种类：字节流（accept / getsockname）用默认的 Tcp，数据报（recvfrom）传 Udp。
std::optional<Endpoint> FromSockaddr(const sockaddr_storage& ss,
                                     socklen_t len,
                                     Endpoint::Kind ip_kind = Endpoint::Kind::Tcp);

// 新建非阻塞、CLOEXEC 的 socket，协议族由 endpoint 决定（IP 类端点是 AF_INET / AF_INET6，UDS 是 AF_UNIX）；
// type 是 SOCK_STREAM / SOCK_DGRAM。
int CreateSocket(const Endpoint& ep, int type, UniqueFd& out);

} // namespace sock

} // namespace breeze
