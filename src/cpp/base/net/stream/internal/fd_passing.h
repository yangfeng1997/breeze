// 在流式 UDS 上随数据传递 fd（SCM_RIGHTS）。共享内存通道的握手用它把 memfd 和 eventfd 交给对端
// （设计见 docs/共享内存IPC设计.md §6.1 / §11）。
//
// 约定：返回 int 的函数返回 errno（0 = 成功），不抛异常。
//
// 安全：对端能往任何一次 sendmsg 里塞 fd。
// - 接收一律带 MSG_CMSG_CLOEXEC，免得收到的 fd 泄漏进 exec 出来的子进程。
// - 收到的 fd 比调用方给的槽位多时，多出来的由内核直接关掉，同时置 MSG_CTRUNC：这里当作协议错误（EMSGSIZE），
//   已收到的 fd 也全部关闭，不留半截。
// - 不期望带 fd 的地方（比如握手的第一条消息）不要用 RecvWithFds，用普通的 recv：没有控制缓冲时内核会丢弃附带的 fd，
//   不会装进本进程的 fd 表。

#pragma once

#include <cstddef>
#include <span>

#include "base/net/socket/unique_fd.h"

namespace breeze::sock
{

// 最多同时传递的 fd 数。握手只用 3 个。
inline constexpr std::size_t kMaxPassedFds = 8;

// 在 sock 上发 data，并把 fds 附在这些数据的第一个字节上。成功返回 0。
// data 要小（握手消息）：一次 sendmsg 没发完时会把剩下的补发，补发遇到发送缓冲满（EAGAIN）就放弃——
// 此时对端已经收到一部分，**调用方必须关闭连接**。任何非 0 的返回之后都应当关闭连接。
// fds 可以为空（等价于带 MSG_NOSIGNAL 的 send）。
int SendWithFds(int sock, std::span<const std::byte> data, std::span<const int> fds);

// 读一次（至多 buf.size() 字节）。读到的数据个数放进 n；读到的 fd 放进 fds_out[0, nfds)，由调用方接管。
// n == 0 且返回 0 表示对端关闭（EOF）；EAGAIN 表示暂时没有数据（n = nfds = 0）。
// fds_out.size() 不得超过 kMaxPassedFds。fd 比槽位多或控制数据被截断：返回 EMSGSIZE，fds_out 里已收到的全部关闭。
int RecvWithFds(int sock, std::span<std::byte> buf, std::size_t& n, std::span<UniqueFd> fds_out, std::size_t& nfds);

} // namespace breeze::sock
