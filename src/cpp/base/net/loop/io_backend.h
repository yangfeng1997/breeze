// 后端按 EventLoopGroup 选择；Auto 只在初始化时探测与回退，不在运行中的连接上切换。
#pragma once

#include <cstdint>

namespace breeze
{

enum class UringReceiveMode : std::uint8_t
{
    Hybrid,
    Multishot,
};

struct UringOptions
{
    UringReceiveMode receive_mode = UringReceiveMode::Hybrid;
    bool owned_vectored_send = false; // 实验项：首帧保留payload引用，可能牺牲小包合并效率。
};

enum class IoBackend : std::uint8_t
{
    Auto,
    Epoll,
    IoUring,
};

constexpr const char* ToString(IoBackend backend) noexcept
{
    switch (backend)
    {
        case IoBackend::Auto:
            return "auto";
        case IoBackend::Epoll:
            return "epoll";
        case IoBackend::IoUring:
            return "io_uring";
    }
    return "invalid";
}

} // namespace breeze
