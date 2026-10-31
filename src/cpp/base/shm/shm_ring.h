// 单向 SPSC 字节环：变长记录，批量发布 / 释放，镜像映射保证记录不被尾部截断。设计见 docs/共享内存IPC设计.md §4 §5。
//
// 角色：每条环恰有一个生产者、一个消费者（可以在不同进程里）。同一时刻同一角色只能有一个活着的对象；
// 角色换了进程（崩溃重启）时，新对象用共享内存里的游标接着用（游标属于角色，不属于进程）。
// 对象本身不是线程安全的：RingProducer / RingConsumer 各自只能由一个线程使用。
//
// 对端不可信（设计文档 §7）：
// - 消费者先把 16 字节记录头拷到本地再校验；游标倒退、占用超过容量、记录越过 write_idx、保留位非零、len 超限，
//   一律锁定为 corrupt（之后一律返回 kCorrupt），不尝试修复。
// - 生产者同样校验消费者写的 read_idx。
// - RecordView::payload 指向共享内存，**对端随时可能改**：要解析就先拷到本地。视图在下一次 Pop 之前有效。
//
// 唤醒（设计文档 §5）：Commit 在发布后检查 consumer_sleeping，需要时敲 eventfd；消费者睡前走 PrepareToSleep，
// 醒来走 WokeUp。两边都是"先写自己的标志、seq_cst 栅栏、再读对方的"（Dekker），不会丢唤醒。
// 睡前自旋与 epoll 由调用方做；本库只给 eventfd 与这两个动作。
//
// 释放策略（设计文档 §6.5）：Pop 立刻推进 read_idx（至多一次）；想批量释放的消费者用 PopNoRelease + Release。
// 消费者处理一条记录前可以 MarkInflight；崩溃后新消费者调 RecoverFromCrash，会跳过那条"处理到一半"的，
// 既不重发，也不会被毒包拖进崩溃循环。

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "shm_defs.h"

namespace breeze::shm
{

// 环的非拥有视图：由 ShmChannel 构造，指向它的映射。
struct RingView
{
    RingControl* control = nullptr;
    std::byte* data = nullptr;  // 镜像映射的起始地址，[0, 2 × capacity) 可访问
    std::uint64_t capacity = 0; // 2 的幂
    std::uint32_t max_payload = 0;
    int eventfd = -1; // < 0 则不唤醒（只用环本身时）
};

enum class ReserveStatus : std::uint8_t
{
    kOk,
    kFull,     // 暂时没有空间；由调用方决定丢、重试还是上报，环自己不阻塞也不静默丢
    kTooLarge, // len 超过 max_payload，永远放不下
    kCorrupt,  // 对端写坏了游标；通道应当断开
};

struct Reserved
{
    ReserveStatus status = ReserveStatus::kFull;
    std::span<std::byte> payload; // kOk 时是 len 字节的可写内存，位于共享内存里；Commit 之前对消费者不可见
};

class RingProducer
{
public:
    RingProducer() = default;
    // 接着共享内存里已有的 write_idx 往下写；游标不合法则 corrupt() 为真。
    explicit RingProducer(const RingView& view);

    // 预留一条 len 字节载荷的记录，同时写好记录头（enqueue_ts_ns 取这一批第一条的时刻，比真实入队略早，
    // 算出的排队年龄偏大，只会早丢、不会晚丢）。可以连续预留多条再一次 Commit。
    Reserved TryReserve(std::uint32_t len) noexcept;

    // 发布自上次 Commit 以来预留的所有记录，并按需唤醒消费者。没有待发布的记录时什么也不做。
    void Commit() noexcept;

    bool corrupt() const noexcept { return corrupt_; }
    std::uint32_t pending_records() const noexcept { return pending_records_; }

private:
    RingView v_;
    std::uint64_t mask_ = 0;
    std::uint64_t w_ = 0;         // 已预留（含未发布）的写游标
    std::uint64_t published_ = 0; // 已发布的写游标，等于共享内存里的 write_idx
    std::uint64_t cached_r_ = 0;  // 上次看到的 read_idx；空间不够时才重新读
    std::uint64_t batch_ts_ = 0;  // 当前批次的入队时刻；0 表示还没开批
    std::uint32_t pending_records_ = 0;
    std::uint64_t pending_bytes_ = 0;
    bool corrupt_ = false;
};

struct RecordView
{
    const std::byte* payload = nullptr; // 指向共享内存，对端可能随时改；解析前先拷到本地
    std::uint32_t len = 0;
    std::uint64_t enqueue_ts_ns = 0;
    std::uint64_t cursor = 0; // 这条记录的起点游标，MarkInflight 用
};

enum class PeekStatus : std::uint8_t
{
    kOk,
    kEmpty,
    kCorrupt,
};

enum class RecoverStatus : std::uint8_t
{
    kClean,   // 上一任没有留下处理到一半的记录
    kSkipped, // 跳过了一条（已推进 read_idx，计入 inflight_skipped）
    kCorrupt, // inflight_idx 或那条记录不合法
};

class RingConsumer
{
public:
    RingConsumer() = default;
    // 从共享内存里的 read_idx 接着读；游标不合法则 corrupt() 为真。
    explicit RingConsumer(const RingView& view);

    // 看下一条记录，不推进。每次 Peek 都会重新校验。
    PeekStatus Peek(RecordView& out) noexcept;

    // 释放最近一次 Peek 到的那条记录，并立刻发布 read_idx。没有成功的 Peek 时什么也不做。
    void Pop() noexcept;
    // 只在本地推进，不发布 read_idx；攒几条之后调 Release。
    void PopNoRelease() noexcept;
    void Release() noexcept;

    // 处理这条记录之前标记；Pop 之后标记自然落后于 read_idx，不用清。
    // 因背压等原因决定先不处理时，要 ClearInflight，否则此时崩溃会把没处理过的记录跳掉。
    void MarkInflight(const RecordView& v) noexcept;
    void ClearInflight() noexcept;

    // 新消费者接手后调用一次（上一任可能崩溃）：inflight_idx 等于 read_idx 说明上一任死在处理这条记录的期间，跳过它。
    RecoverStatus RecoverFromCrash() noexcept;

    // 睡前：置 consumer_sleeping、栅栏、回头再看一眼。返回 true 才可以去等 eventfd；
    // 返回 false 说明这个窗口里有记录进来了，已经把标志清回去，继续干活。
    bool PrepareToSleep() noexcept;
    // 醒来之后（不论真假唤醒）：清标志、读清 eventfd。
    void WokeUp() noexcept;

    int eventfd() const noexcept { return v_.eventfd; }
    bool corrupt() const noexcept { return corrupt_; }

private:
    RingView v_;
    std::uint64_t mask_ = 0;
    std::uint64_t cursor_ = 0;   // 本地读游标：下一条要读的记录
    std::uint64_t last_w_ = 0;   // 见过的最大 write_idx，用来发现倒退
    std::uint64_t cur_size_ = 0; // 最近一次 Peek 的记录占用；0 表示没有可 Pop 的
    std::uint32_t cur_len_ = 0;
    bool corrupt_ = false;
};

} // namespace breeze::shm
