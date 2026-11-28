// EventLoop 的内部 IO 引擎；readiness 集服务传统fd，也用于流读取的有界准入。
// 上层字节/数据报仍由完成回调交付，不能把CQE伪装成可读事件后再read。
#pragma once

#include <cstdint>
#include <memory>
#include <span>

#include "base/buffer/shared_buffer.h"
#include "base/net/loop/io_backend.h"
#include "base/net/loop/io_completion.h"
#include "base/net/loop/io_stats.h"
#include "base/net/loop/pollable_interface.h"

namespace breeze
{

struct IoEvent
{
    IPollable* target;
    std::uint32_t events;
};

class IoEngine
{
public:
    virtual ~IoEngine() = default;
    IoEngine(const IoEngine&) = delete;
    IoEngine& operator=(const IoEngine&) = delete;

    [[nodiscard]] virtual int Init() = 0;
    virtual IoBackend backend() const noexcept = 0;
    [[nodiscard]] virtual int Add(IPollable* target, int fd, std::uint32_t events) = 0;
    virtual void Mod(IPollable* target, int fd, std::uint32_t events) = 0;
    virtual void Del(int fd) = 0;

    // 返回已取出的事件数；EINTR 返回 0，永久错误返回负 errno，调用方不得静默空转。
    virtual int Wait(std::span<IoEvent> events, int timeout_ms) = 0;
    // Wait 只收割；Dispatch 在 loop 清 sleeping 标志、更新时间后执行回调。
    virtual void Dispatch() {}
    virtual void Flush() {}
    // loop 已停止接纳新连接，退出线程前等待取消完成，不能把在途 fd 留到 group 析构。
    virtual void Drain() {}
    virtual IoStats stats() const noexcept { return {}; }
    std::weak_ptr<void> lifetime() const noexcept { return lifetime_; }

    virtual IoRegistration AttachStream(int fd, IStreamCompletion& sink, int& error);
    virtual void ReadStream(IoRegistration id, bool enabled);
    // 引擎复制并持有提交字节，Detach 后连接可以销毁；完成回调不会同步发生。
    virtual int WriteStream(IoRegistration id, std::span<const iovec> buffers);
    // owned字节可直接借给内核；引擎持有引用至终结CQE，调用方可以立即销毁连接。
    virtual int WriteOwnedStream(IoRegistration id, SharedBuffer bytes);
    virtual void Detach(IoRegistration id);
    virtual IoRegistration AttachDatagram(int fd, IDatagramCompletion& sink, int& error);
    // 数据报提交时复制；0=引擎接收，不承诺对端交付，异步错误经 sink 上报。
    virtual int SendDatagram(IoRegistration id,
                             std::span<const std::byte> prefix,
                             std::span<const std::byte> payload,
                             const sockaddr_storage* to,
                             socklen_t length);

protected:
    IoEngine() = default;

private:
    std::shared_ptr<void> lifetime_ = std::make_shared<int>(0);
};

[[nodiscard]] std::unique_ptr<IoEngine> MakeIoEngine(IoBackend backend, int& error);

} // namespace breeze
