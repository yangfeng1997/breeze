// App 一侧：连上 Agent 的 UDS，握手，拿到三个 fd 并接手共享内存通道。设计见 docs/共享内存IPC设计.md §6.1 / §6.2 / §6.5。
//
// Connect / Reattach 是**阻塞**的（连接 + 发 Hello + 等 Reply，整体受 timeout 限制），适合进程启动或后台线程里调用，
// 不要在主循环里调：Agent 不在时它会一直等到超时。
//
// 三种场景：
//   第一次启动      Connect()：Agent 新建通道。
//   App 重启        Connect()：同一个 busid 在保留期内再来，Agent 把原来的 memfd 再传一遍，resumed() 为真，
//                   Rx 里是重启期间积压的消息，Tx 的游标接着用。退出前可以 SendPrepareRestart 要更长的保留，
//                   正常退出发 SendBye 让 Agent 排空就销毁。
//   Agent 重启      AgentGone() 为真之后，Reattach()：把手里的 fd 传回给新 Agent；本对象的映射、tx() / rx() 原样接着用，
//                   Tx 环里 Agent 不在期间写的记录由新 Agent 继续处理。
//
// 握手之后 UDS 只剩两个用途：探活（Agent 退出时它变为可读，EOF）与向 Agent 发控制消息。所以把 liveness_fd() 挂进 epoll
// （或定期问 AgentGone()）就能发现 Agent 没了。
//
// 通道本身怎么用见 shm_channel.h 与 shm_ring.h：tx() 是 App → Agent 的生产者，rx() 是 Agent → App 的消费者；
// rx().eventfd() 挂进主循环的 epoll；睡前调 rx().PrepareToSleep()，醒来调 rx().WokeUp()。
// **Connect / Reattach 成功之后要先把 Rx 无条件读一遍再睡**：上一个进程可能死在睡着之后，睡眠标志停在 1。
//
// 对象可以移动，tx() / rx() 返回的引用在移动之后失效（重新取）；本对象必须比从它取出的东西活得久。

#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <string>

#include "base/net/endpoint/endpoint.h"
#include "base/net/socket/unique_fd.h"
#include "base/shm/shm_channel.h"
#include "base/shm/shm_ring.h"

namespace breeze
{

struct ShmClientOptions
{
    Endpoint endpoint = *Endpoint::Unix("/run/breeze/agent.sock"); // Agent 的 UDS：Unix 或 UnixAbstract
    std::string busid; // 通道名，[A-Za-z0-9._:@-]{1,63}，同一个 Agent 上唯一
    // 0 = 默认（16 MiB）；其余必须是 2 的幂且在 [1 MiB, Agent 的单环上限]，否则 Agent 回 Reject(EINVAL)。
    std::uint64_t tx_capacity = 0;
    std::uint64_t rx_capacity = 0;
    std::uint32_t tx_max_payload = 0; // 0 = 默认
    std::uint32_t rx_max_payload = 0;
    // 整个 Connect / Reattach 的时限（连接、发送、等回复合计）。
    std::chrono::milliseconds timeout{3000};
    // 收到 EBUSY（通道名被占）后的重试窗口：App 快速重启时 Agent 可能还没处理完旧连接，稍等就好。窗口之外仍是 EBUSY 就返回。0 = 不重试。
    std::chrono::milliseconds busy_retry{1000};
    // Connect 才有：丢弃 Agent 保留着的同名队列，按上面的容量重建（想换环的大小时用）。
    bool discard_retained = false;
    // 接手时对 Agent 给的容量设的上限（Agent 不可信到什么程度由你定）：它给的比这大就拒绝。
    shm::AttachLimits limits;
};

class ShmChannelClient
{
public:
    ShmChannelClient() = default;
    ShmChannelClient(ShmChannelClient&&) noexcept = default;
    ShmChannelClient& operator=(ShmChannelClient&&) noexcept = default;
    ShmChannelClient(const ShmChannelClient&) = delete;
    ShmChannelClient& operator=(const ShmChannelClient&) = delete;

    // 返回 errno：
    //   EINVAL           通道名不合法，或 Agent 拒绝了容量参数 / Hello（Reject 的原因原样返回）
    //   EPROTONOSUPPORT  端点不是 UDS
    //   ENOENT / ECONNREFUSED  Agent 没在监听
    //   ETIMEDOUT        超时
    //   EBUSY            同名通道已存在且 App 在线（busy_retry 窗口内会重试，过了窗口还占着才返回）
    //   EACCES           身份不被允许；保留着的队列只还给同一个用户
    //   ENOSPC           Agent 的通道数或共享内存总量已到配额
    //   EACCES           身份不被允许
    //   EPROTO           对端不是我们的协议、版本不符，或回复与共享内存里的布局对不上
    //   ENOTSUP          Agent 不支持这种挂载
    static std::expected<ShmChannelClient, int> Connect(const ShmClientOptions& options);

    // Agent 重启之后：把手里的 memfd 与两个 eventfd 传回新 Agent，通道原样接着用。返回 errno（含 Connect 的那些）：
    //   EINVAL           Agent 校验这块内存不过（没封印、头损坏、游标越界、容量超过它的上限……），或本对象没有通道
    //   ESTALE           通道已被别的进程接管（app_epoch 对不上）：本进程应当退出
    //   EPROTO           Agent 的回复与手里的内存对不上
    // 成功之后 sock_ 换成新连接，agent_epoch() 更新；失败时本对象保持原样（仍可再试）。
    // 没有真的断开时也可以调：Agent 若还留着这条通道，会认出这是同一块内存，原样恢复。
    int Reattach(const ShmClientOptions& options);

    // 控制消息（App → Agent，经 UDS）。返回 errno；成功只表示发出去了，Agent 不回确认。
    // PrepareRestart：App 马上要重启，希望 Agent 把队列留 retain 这么久（Agent 有上限）；用于热更新。
    // Bye：App 正常退出，Agent 排空 Tx 之后立刻销毁，不保留。
    int SendPrepareRestart(std::chrono::milliseconds retain);
    int SendBye();

    bool valid() const noexcept { return chan_.valid(); }
    shm::ShmChannel& shm() noexcept { return chan_; }
    shm::RingProducer& tx() noexcept { return tx_; } // App → Agent
    shm::RingConsumer& rx() noexcept { return rx_; } // Agent → App
    std::uint64_t app_epoch() const noexcept { return app_epoch_; }
    std::uint64_t agent_epoch() const noexcept { return agent_epoch_; }
    // 最近一次 Connect / Reattach 是不是恢复了保留着的队列（false = 全新通道）。
    bool resumed() const noexcept { return resumed_; }
    // 共享内存头里的 app_epoch 已经不是本进程拿到的那个值：别的进程接管了这个通道，本进程应当退出。
    bool Superseded() const noexcept;

    // 握手用的 UDS，之后只做探活：Agent 退出时它变为可读（EOF）。可以挂进 epoll（EPOLLIN | EPOLLRDHUP）。
    int liveness_fd() const noexcept { return sock_.get(); }
    // 非阻塞探测：true = Agent 已经断开（读到 EOF、出错，或收到了不该有的数据）。
    bool AgentGone() noexcept;

private:
    UniqueFd sock_;
    shm::ShmChannel chan_;
    shm::RingProducer tx_;
    shm::RingConsumer rx_;
    std::uint64_t app_epoch_ = 0;
    std::uint64_t agent_epoch_ = 0;
    bool resumed_ = false;
};

} // namespace breeze
