// 后端按 EventLoopGroup 选择；Auto 只在初始化时探测与回退，不在运行中的连接上切换。
#pragma once

#include <cstdint>

namespace breeze
{

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
