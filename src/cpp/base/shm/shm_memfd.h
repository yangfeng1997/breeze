// memfd 与 eventfd 的创建和校验。错误一律返回 errno，不抛异常、不写日志。
//
// 为什么要封印：对端能 ftruncate 缩小共享的 memfd，另一边一访问被截掉的页就收到 SIGBUS 崩溃。
// F_SEAL_SHRINK | F_SEAL_GROW 之后大小就定死了；F_SEAL_SEAL 防止对端再改封印（设计文档 §6.1）。
// 封印不限制写内容，数据照常读写。

#pragma once

#include <cstdint>
#include <expected>

#include "shm_fd.h"

namespace breeze::shm
{

// 创建 memfd（MFD_CLOEXEC | MFD_ALLOW_SEALING）并 ftruncate 到 size，**未封印**：创建方先初始化内容，再调 SealMemFd。
std::expected<Fd, int> CreateMemFd(const char* name, std::uint64_t size);

// 加上 SHRINK | GROW | SEAL 三个封印。成功返回 0。
int SealMemFd(int fd);

// 校验 fd 是封印齐全的 memfd，并取出它的大小。接手对端交来的 fd 前必须调用。
// 不是 memfd（不支持封印）、封印不全、不是普通文件都返回 EINVAL。
int CheckSealedMemFd(int fd, std::uint64_t* size_out);

// 创建 EFD_NONBLOCK | EFD_CLOEXEC 的 eventfd。
std::expected<Fd, int> CreateEventFd();

// 校验 fd 是非阻塞的 eventfd。对端交来的 fd 可能是别的东西：写一个阻塞的管道会让 Agent 卡住。
// 靠 /proc/self/fd 的链接目标判断，/proc 不可用时无法确认，按失败（EINVAL）处理。
// proc_fd_dir 可以换成别的目录，只给测试用：模拟没有 /proc 的环境。
int CheckEventFd(int fd, const char* proc_fd_dir = "/proc/self/fd");

} // namespace breeze::shm
