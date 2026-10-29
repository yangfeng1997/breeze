// 连接句柄：64 位值 [group:4 | loop:8 | slot:20 | generation:32]，不含指针，可平凡复制，任意线程可用。
// 设计见 docs/网络库设计.md §4.3。
//
// 解析顺序：按 group 位在进程级 GroupRegistry 里找 group（空 → Closed）→ 校验 loop / slot 是否越界 →
// 校验槽的 generation 与状态并累加 pending_bytes → Post 给所属 loop；任务在 loop 线程上执行时还会再校验一次 generation，
// 所以迟到的消息 / 事件不会串到复用了同一个槽的新连接上。
//
// 限制：每进程 ≤16 个 group，每 group ≤256 个 loop，每 loop ≤2^20 个连接；generation 从 1 起，
// 所以 raw == 0 恒为无效句柄（NetClient 的原子句柄用 0 表示"无连接"）。
// group 销毁后其句柄作废：解析不到时返回 Closed；使用方须在销毁 group 之前丢弃它的句柄，并保证销毁时没有线程仍在 Send。

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "send_result.h"
#include "shared_buffer.h"

namespace breeze
{

class ConnectionHandle
{
public:
    static constexpr std::uint32_t kMaxGroups = 16;
    static constexpr std::uint32_t kMaxLoops = 256;
    static constexpr std::uint32_t kMaxSlots = 1u << 20;

    constexpr ConnectionHandle() noexcept = default;
    explicit constexpr ConnectionHandle(std::uint64_t raw) noexcept
        : raw_(raw)
    {
    }

    // 各字段须在上面的范围内；generation 为 0 得到无效句柄。
    static constexpr ConnectionHandle Make(
        std::uint32_t group, std::uint32_t loop, std::uint32_t slot, std::uint32_t generation) noexcept
    {
        return ConnectionHandle((static_cast<std::uint64_t>(group) << 60) | (static_cast<std::uint64_t>(loop) << 52) |
                                (static_cast<std::uint64_t>(slot) << 32) | generation);
    }

    constexpr std::uint64_t raw() const noexcept { return raw_; }
    constexpr bool valid() const noexcept { return raw_ != 0; }
    constexpr std::uint32_t group() const noexcept { return static_cast<std::uint32_t>(raw_ >> 60); }
    constexpr std::uint32_t loop() const noexcept { return static_cast<std::uint32_t>((raw_ >> 52) & 0xFF); }
    constexpr std::uint32_t slot() const noexcept { return static_cast<std::uint32_t>((raw_ >> 32) & 0xFFFFF); }
    constexpr std::uint32_t generation() const noexcept { return static_cast<std::uint32_t>(raw_); }

    friend constexpr bool operator==(ConnectionHandle a, ConnectionHandle b) noexcept = default;

    // 日志用：g<group>.l<loop>#<slot>.<generation>
    std::string ToString() const;

    // ---- 线程安全，任意线程可调 ----

    // 帧头由连接的 codec 生成，payload 不拷贝。Ok 只表示网络层已接收；Closed = 句柄已过期、连接已关闭或正在关闭，
    // 或本次发送使排队字节超过 HighWaterMark（此时连接以 SlowConsumer 关闭）。
    [[nodiscard]] SendResult Send(SharedBuffer payload) const;
    // 立即关闭，OnClosed 的原因是 LocalClose。对已过期的句柄是空操作。
    void Close() const;
    // 优雅关闭（语义见 Connection::CloseGracefully）：此前经同一线程 Send 的数据先发完，再 shutdown(SHUT_WR)，
    // 对端未在 timeout 内关闭则强关。timeout 超过 49 天按 49 天算。
    void CloseGracefully(std::chrono::milliseconds timeout) const;
    // 摘掉 / 恢复该连接的 EPOLLIN（语义见 Connection::PauseRead）。异步：投递给所属 loop，返回时尚未生效。
    void PauseRead() const;
    void ResumeRead() const;

private:
    std::uint64_t raw_ = 0;
};

} // namespace breeze

template <>
struct std::hash<breeze::ConnectionHandle>
{
    std::size_t operator()(breeze::ConnectionHandle h) const noexcept { return std::hash<std::uint64_t>{}(h.raw()); }
};
