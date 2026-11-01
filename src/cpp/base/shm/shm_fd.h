// 拥有一个 fd 的 RAII 句柄：只能移动，析构时关闭。
//
// 与 base/net/socket/unique_fd.h 重复：base/shm 不能依赖 net 的内部头，
// 等 UniqueFd 上提到 base 之后再合并。

#pragma once

#include <unistd.h>

namespace breeze::shm
{

class Fd
{
public:
    Fd() noexcept = default;
    explicit Fd(int fd) noexcept
        : fd_(fd)
    {
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& o) noexcept
        : fd_(o.release())
    {
    }
    Fd& operator=(Fd&& o) noexcept
    {
        if (this != &o)
            reset(o.release());
        return *this;
    }
    ~Fd() { reset(); }

    int get() const noexcept { return fd_; }
    bool valid() const noexcept { return fd_ >= 0; }
    int release() noexcept
    {
        const int f = fd_;
        fd_ = -1;
        return f;
    }
    void reset(int fd = -1) noexcept
    {
        if (fd_ >= 0)
            ::close(fd_);
        fd_ = fd;
    }

private:
    int fd_ = -1;
};

} // namespace breeze::shm
