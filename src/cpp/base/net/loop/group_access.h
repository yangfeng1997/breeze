// EventLoopGroup 的内部入口：公开头里 EventLoop 只是前置声明，需要拿到 loop 的内部代码（句柄解析、StreamServer、测试）
// 通过这里取，不需要把访问器放进公开接口。

#pragma once

#include <cstddef>
#include <cstdint>

#include "base/net/loop/event_loop.h"
#include "base/net/loop/event_loop_group.h"

namespace breeze
{

struct GroupAccess
{
    static std::size_t LoopCount(const EventLoopGroup& g) noexcept;
    // 下标越界返回空。任意线程可调（Running 期间 workers_ 不变）。
    static EventLoop* TryLoopAt(const EventLoopGroup& g, std::size_t i) noexcept;
    // 下标越界是编程错误。
    static EventLoop& LoopAt(const EventLoopGroup& g, std::size_t i);
    static std::uint32_t Id(const EventLoopGroup& g) noexcept { return g.id_; }
    // 连接数最少的 loop（读各表的 Live()，并发时只是近似）。group 不在运行是编程错误。
    static EventLoop& PickLoop(const EventLoopGroup& g);
};

} // namespace breeze
