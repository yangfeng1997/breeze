// 一条已建立的连接（接口）：业务回调（ConnectionHandler）里拿到的就是它。设计见 docs/网络库设计.md §4.4。
//
// 具体类：StreamConnection（TCP / UDS，内部类型）；将来 RUdpConnection。业务代码只认这个接口，不关心底下是什么传输。
// 只能在所属 EventLoop 的 IO 线程上使用（回调里拿到的 Connection& 即是）；跨线程用 ConnectionHandle。
//
// **只有虚函数、没有数据成员**：具体类里热字段的布局是按缓存行排过的（见 stream_connection.h），基类带数据会让它们
// 整体后移。所以对端、句柄、上下文这些访问器也是虚函数，状态由各实现自己持有。
// 继承链是 Connection → Pollable，vptr 只有一个，对象布局与抽象之前完全一样。Pollable 是"注册到 EventLoop 的 fd 的
// 事件接收者"；没有自己 fd 的传输（共用一个 UDP socket 的会话）实现 OnEvents 时不会被调用，留空即可。
// 抽象的成本见 docs/网络库设计.md §9.5 / §9.6。

#pragma once

#include <cassert>
#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>

#include "connection_context.h"
#include "connection_handle.h"
#include "connection_handler.h"
#include "internal/core/pollable.h"
#include "peer_info.h"
#include "send_result.h"
#include "shared_buffer.h"

namespace breeze
{

class EventLoop;

class Connection : protected Pollable
{
public:
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    // 发送一帧：帧头由本连接的 codec 生成，payload 不拷贝（同一块可以同时发给多条连接）。
    // IO 线程内写队列为空时先直接写，写不完才入队并注册 EPOLLOUT；已有积压则排在后面，保序。
    virtual SendResult Send(SharedBuffer payload) = 0;

    // 视图版本：不拿走所有权，也不要求调用方先拷成 SharedBuffer。尽量直接从 payload
    // 写出（写队列为空时整帧写完就一次拷贝都没有）， 写不完的部分（或前面已有积压、必须排队保序时）才把 payload 拷成
    // SharedBuffer 入队——所以返回之后 payload 指向的内存可以立刻改写或释放。 给在 IO 线程里立即回包的 handler
    // 用（echo、小的应答、栈上拼出来的响应）：OnMessage 的 payload 只在回调内有效， 以前只能先 CopyFrom 再
    // Send，每条消息白白多一次分配与拷贝。已经有 SharedBuffer（要跨连接共享、要入队）时用上面那个。
    virtual SendResult Send(std::span<const std::byte> payload) = 0;

    // 立即关闭。连接上不再有读写，OnClosed 在本轮分发之后回调（不在当前调用栈里），对象此后才销毁。
    // 重复调用、对已关闭的连接调用都是空操作。
    virtual void Close() = 0;

    // 优雅关闭：转入 Disconnecting（Send 从此返回 Closed），发送队列排空后 shutdown(SHUT_WR)，同时启动 timeout 定时器；
    // 对端在期限内关闭则正常结束，否则强关。两种结果 OnClosed 的原因都是 LocalClose。
    // 期间仍读、仍分发 OnMessage（对端在 FIN 之前发来的最后几条消息不丢），并忽略 PauseRead（已暂停的会先恢复读）。
    // 对非 Connected 的连接是空操作。
    virtual void CloseGracefully(std::chrono::milliseconds timeout) = 0;

    // 摘掉 / 恢复 EPOLLIN，流控用（MainLoopExecutor 的入口背压）。只影响之后的 read：本次 read 已经读进来的帧照常交付
    // （最多 64KB），所以暂停是软的、超出量有界。暂停期间不计空闲，恢复时重新计时；重复调用是空操作。
    virtual void PauseRead() = 0;
    virtual void ResumeRead() = 0;

    // 每连接的用户数据，随连接销毁。Release 下 GetContext 是 static_cast，Debug 下 dynamic_cast 并断言类型对得上。
    virtual void SetContext(std::unique_ptr<ConnectionContext> ctx) noexcept = 0;
    template <class T>
    T* GetContext() const noexcept
    {
        static_assert(std::is_base_of_v<ConnectionContext, T>);
#ifdef NDEBUG
        return static_cast<T*>(context());
#else
        T* p = dynamic_cast<T*>(context());
        assert(p != nullptr || context() == nullptr); // 取成了别的类型
        return p;
#endif
    }

    virtual bool IsConnected() const noexcept = 0;
    virtual bool IsPaused() const noexcept = 0;
    virtual const PeerInfo& peer() const noexcept = 0;

    // 可复制、可跨线程使用的句柄（ConnectionHandle::Send / Close）。独立 loop（不属于任何 group）上的连接没有句柄，
    // 此时返回无效句柄（raw == 0）。
    virtual ConnectionHandle handle() const noexcept = 0;

    ~Connection() override = default;

protected:
    Connection() = default;

    // GetContext 的底层：没设过上下文返回空。
    virtual ConnectionContext* context() const noexcept = 0;

private:
    friend class EventLoop; // 任务执行者与回收者：下面这些只由它调用

    // 只由 EventLoop 在执行跨线程任务时调用。
    virtual void SendFromTask(SharedBuffer payload) = 0;
    virtual void CloseFromTask(CloseReason reason) = 0;
    // 只由 EventLoop 的空闲扫描调用。
    virtual void CloseIdle() = 0;
    // 只由 EventLoop 在回收时调用：回调 OnClosed。
    virtual void NotifyClosed() = 0;
    // 在 ConnectionTable 里的槽下标，回收时用。
    virtual std::uint32_t slot_index() const noexcept = 0;
};

} // namespace breeze
