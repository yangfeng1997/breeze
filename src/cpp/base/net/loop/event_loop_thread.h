// 在独立线程上跑一个 EventLoop：测试与 bench 的装配用。将来由 EventLoopGroup（拥有 N 个 loop 与线程）取代。
//
// 用法：先 Init，在 loop 线程开始运行之前（装配阶段）创建 Acceptor / LoopServer 并 Start，再 Start() 起线程。

#pragma once

#include <cstdio>
#include <cstdlib>

#include "base/net/loop/event_loop.h"
#include "base/thread/thread.h"

namespace breeze
{

class EventLoopThread
{
public:
    explicit EventLoopThread(const EventLoopOptions& options = {})
    {
        if (loop.Init(options) != 0)
        {
            std::fprintf(stderr, "EventLoopThread: EventLoop::Init 失败\n");
            std::abort();
        }
    }
    ~EventLoopThread() { Stop(); }

    EventLoopThread(const EventLoopThread&) = delete;
    EventLoopThread& operator=(const EventLoopThread&) = delete;

    void Start()
    {
        if (thread_.Start([this] { loop.Run(); }, "loop") != 0)
        {
            std::fprintf(stderr, "EventLoopThread: 创建线程失败\n");
            std::abort();
        }
    }

    // 让 loop 退出并 join。重复调用、没启动过都是空操作。
    void Stop()
    {
        if (!thread_.Joinable())
            return;
        loop.Quit();
        thread_.Join();
    }

    pthread_t native_handle() const noexcept { return thread_.native_handle(); }

    EventLoop loop;

private:
    Thread thread_;
};

} // namespace breeze
