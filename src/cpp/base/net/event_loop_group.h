// N 个 EventLoop + N 个线程。设计见 docs/网络库设计.md §4.1 / §4.3 / §4.8。
//
// 状态 Idle → Running → Stopped，各转换只发生一次：Start 只能在 Idle 调用，Shutdown 之后不能再 Start（编程错误，LOG_FATAL）。
// Start / Shutdown 由同一个线程（通常是 main）调用，不并发。
//
// 谁创建 IO 线程，谁在 Shutdown / 析构里 join。析构只做兜底：没 Shutdown 过就直接停（不排空）。

#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

namespace breeze
{

class EventLoop;

struct EventLoopGroupOptions
{
    std::uint32_t io_threads = 1;                    // 1 到 256
    std::uint32_t max_connections_per_loop = 65536;  // 1 到 2^20；每个 loop 的连接表按它预分配（每槽 64 字节）
};

class EventLoopGroup
{
public:
    explicit EventLoopGroup(EventLoopGroupOptions options = {});
    ~EventLoopGroup();

    EventLoopGroup(const EventLoopGroup&) = delete;
    EventLoopGroup& operator=(const EventLoopGroup&) = delete;

    // 0 = 成功，否则 errno：选项越界 EINVAL；注册表已满 ENOSPC；创建 loop / 线程失败时是对应的 errno。
    // 失败时已回滚：已起的线程全部 join，已开的 fd 全部关闭，注册表名额已归还。
    [[nodiscard]] int Start();

    // 停掉所有 loop 并 join 线程；之后本 group 的句柄解析不到（Send 返回 Closed）。
    // 对没有成功 Start 的对象、以及重复调用都是安全的空操作。
    void Shutdown(std::chrono::milliseconds deadline);

    bool running() const noexcept { return state_ == State::Running; }

private:
    friend struct GroupAccess; // 内部与测试通过 internal/group_access.h 取 loop

    enum class State : std::uint8_t
    {
        Idle,
        Running,
        Stopped,
    };
    struct Worker; // EventLoop + Thread，定义在 .cpp

    void StopWorkers(std::size_t started);

    EventLoopGroupOptions options_;
    State state_ = State::Idle;
    std::uint32_t id_ = 0; // 在 GroupRegistry 里的编号
    std::vector<std::unique_ptr<Worker>> workers_;
};

} // namespace breeze
