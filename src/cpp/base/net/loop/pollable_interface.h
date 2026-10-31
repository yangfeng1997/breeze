// 注册到 EventLoop 的 fd 的事件接收者（Acceptor、IConnection、唤醒用的 eventfd）。

#pragma once

#include <cstdint>

namespace breeze
{

class IPollable
{
public:
    virtual ~IPollable() = default;
    // events 是 epoll 返回的事件位（EPOLLIN / EPOLLOUT / EPOLLERR / EPOLLHUP / EPOLLRDHUP）。
    virtual void OnEvents(std::uint32_t events) = 0;
};

} // namespace breeze
