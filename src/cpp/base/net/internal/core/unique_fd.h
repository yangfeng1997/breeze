// 拥有一个 fd 的 RAII 句柄：只能移动，析构时关闭。所有传输共用。

#pragma once

namespace breeze
{

// 拥有一个 fd；析构时关闭。
class UniqueFd
{
public:
    UniqueFd() noexcept = default;
    explicit UniqueFd(int fd) noexcept
        : fd_(fd)
    {
    }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& o) noexcept
        : fd_(o.release())
    {
    }
    UniqueFd& operator=(UniqueFd&& o) noexcept
    {
        if (this != &o)
            reset(o.release());
        return *this;
    }
    ~UniqueFd() { reset(); }

    int get() const noexcept { return fd_; }
    bool valid() const noexcept { return fd_ >= 0; }
    int release() noexcept
    {
        const int f = fd_;
        fd_ = -1;
        return f;
    }
    void reset(int fd = -1) noexcept;

private:
    int fd_ = -1;
};

} // namespace breeze
