// 字节流（TCP / UDS）socket 的薄封装：建立、监听、连接、accept。设计见 docs/网络库设计.md §4.2 / §4.7。
//
// 约定：返回 int 的函数返回 errno（0 = 成功），不抛异常；失败时输出参数保持不变。
// Connection 以上不出现 if tcp：TCP / UDS 的差异收在 tcp/ 与 uds/，这里只在 Bind 处按类型分派一次。

#pragma once

#include <optional>

#include "endpoint.h"
#include "unique_fd.h"

namespace breeze
{

namespace sock
{

// 新建流式 socket（非阻塞、CLOEXEC），协议族由 endpoint 决定。
int CreateStreamSocket(const Endpoint& ep, UniqueFd& out);

// UDS 路径会先探测残留的 socket 文件（uds::ClearStaleSocket），再 bind。
int Bind(int fd, const Endpoint& ep);
int Listen(int fd, int backlog);

// 非阻塞 connect：0 = 已连上；EINPROGRESS = 进行中，等 EPOLLOUT 后用 GetSocketError 判定；其它为失败。
int Connect(int fd, const Endpoint& ep);

// accept4（非阻塞、CLOEXEC）。EAGAIN = 没有更多待接受的连接。peer 为对端地址，未命名的 UDS 对端为空。
int Accept(int listen_fd, UniqueFd& out, std::optional<Endpoint>& peer);

// 读 SO_ERROR 并清除。返回该 errno（0 = 没有错误）；getsockopt 自己失败时返回它的 errno。
int GetSocketError(int fd);

int LocalEndpoint(int fd, std::optional<Endpoint>& out);
int PeerEndpoint(int fd, std::optional<Endpoint>& out);

} // namespace sock

} // namespace breeze
