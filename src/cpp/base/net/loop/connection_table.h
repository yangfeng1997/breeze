// 每个 EventLoop 一张的连接表：generational slot map。设计见 docs/网络库设计.md §4.3。
//
// 槽分两半：热字段（generation / state / pending_bytes / high_water）是原子量，任意线程可读写，一槽一个缓存行，
// 免得别的线程更新 pending_bytes 时拖慢 IO 线程更新相邻槽；IConnection 指针与空闲环只有所属 IO 线程碰，另放。
//
// 线程规则：Find / TryReserve / Unreserve / Live / PendingBytes 任意线程可调；其余只能在所属 loop 线程。
// 校验与累加不是原子的（Find 之后槽可能被回收复用）：要在两者之间走完整条 FIFO 空闲链才会复用同一个槽，
// 即使发生也只是新连接的 pending_bytes 偏差一条消息的字节数——水位本就是软上限（brpc 同样如此）。

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

namespace breeze
{

class IConnection;

class ConnectionTable
{
public:
    static constexpr std::uint32_t kMaxCapacity = 1u << 20; // 句柄里 slot 占 20 位

    enum class State : std::uint8_t
    {
        Free,
        Connected,
        Disconnecting, // 正在优雅关闭：不再接受 Send，等写队列排空与对端关闭
        Disconnected,  // 已标记关闭，等本轮分发之后回收
    };

    struct alignas(64) Slot
    {
        std::atomic<std::uint32_t> generation{1}; // 从 1 起：句柄 raw == 0 恒为无效；回收时 +1
        std::atomic<State> state{State::Free};
        std::atomic<std::uint64_t> pending_bytes{0}; // 排队与在途的 payload 字节；回收时清零
        std::atomic<std::uint64_t> high_water{0};    // 建立时写入；发送方据此判断慢消费者
    };
    static_assert(sizeof(Slot) == 64);

    // 空闲检测用的 last_active_time：单独的连续数组，只有所属 IO 线程访问，idle 扫描只读它，不碰 IConnection 对象。
    // timeout_ms == 0 表示这个槽不参加空闲检测（空槽，或这条连接没开 idle_timeout）。
    struct IdleEntry
    {
        std::chrono::steady_clock::time_point last_active{};
        std::uint32_t timeout_ms = 0;
        bool paused = false; // 被 PauseRead 暂停：扫描跳过
    };

    explicit ConnectionTable(std::uint32_t capacity);
    ~ConnectionTable();

    ConnectionTable(const ConnectionTable&) = delete;
    ConnectionTable& operator=(const ConnectionTable&) = delete;

    std::uint32_t capacity() const noexcept { return capacity_; }

    // ---- 任意线程 ----

    // 预留一个名额：accept 线程选中这个 loop 时先占位，连接真正建起来（在目标 loop 线程上）之前也算在内，
    // 所以一阵突发 accept 不会因为 Post 还没落地就全挑同一个 loop，也不会超过 capacity。超限返回 false。
    bool TryReserve() noexcept;
    // 预留之后没能建起连接（登记失败、loop 正在停）时归还。
    void Unreserve() noexcept { live_.fetch_sub(1, std::memory_order_relaxed); }
    // 已预留 + 已建立的连接数。
    std::uint32_t Live() const noexcept { return live_.load(std::memory_order_relaxed); }

    // 句柄解析：slot 越界、generation 不符、状态不是 Connected 都返回空。
    Slot* Find(std::uint32_t slot, std::uint32_t generation) noexcept;
    std::uint64_t PendingBytes(std::uint32_t slot) const noexcept;

    // ---- 只能在 loop 线程 ----

    // 取出最早空出来的槽（FIFO，拉长同一槽的复用间隔），状态置 Connected。调用方须已 TryReserve 成功，否则是编程错误。
    std::uint32_t Allocate(std::uint64_t high_water);
    void Install(std::uint32_t slot, std::unique_ptr<IConnection> conn);
    Slot& slot(std::uint32_t i) noexcept { return slots_[i]; }
    // generation 相符且已装了连接才返回；任务在连接关闭之后才到达时用它识别"迟到"。
    IConnection* Current(std::uint32_t slot, std::uint32_t generation) noexcept;
    // 不校验 generation：idle 扫描按槽下标取当前连接。没装连接返回空。
    IConnection* At(std::uint32_t slot) noexcept { return conns_[slot].get(); }
    IdleEntry& idle(std::uint32_t slot) noexcept { return idle_[slot]; }
    // 遍历所有已装的连接（停机时用）。f 里可以关闭连接（回收在本轮分发之后），但不能新建连接。
    template <class F>
    void ForEachConnection(F&& f)
    {
        for (auto& c : conns_)
            if (c)
                f(*c);
    }
    // 销毁连接、generation + 1、清零、放回空闲环尾，并归还名额。
    void Reclaim(std::uint32_t slot);

private:
    std::uint32_t capacity_;
    std::unique_ptr<Slot[]> slots_;
    std::vector<std::unique_ptr<IConnection>> conns_;
    std::vector<IdleEntry> idle_;
    std::vector<std::uint32_t> free_; // 环形队列，容量 = capacity_
    std::uint32_t free_head_ = 0;
    std::uint32_t free_count_ = 0;
    std::atomic<std::uint32_t> live_{0};
};

} // namespace breeze
