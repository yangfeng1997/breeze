// ikcp 的薄封装：一个 KCP 会话（消息模式，保持消息边界）。设计见 docs/网络库设计.md §4.14。
//
// 为什么封装：ikcp 是 C 接口，返回值约定五花八门（0 / 长度 / 负数混用），时钟是 32 位毫秒，输出走裸函数指针。
// 这里统一成 C++ 类型，并把"消息最多多少段"之类 KCP 的约束固定下来；上层（RudpConnection）只认这里。
//
// 非线程安全，只在会话所属的 loop 线程上用。output 回调在 Update / Flush / Send（flush_on_send 时）里同步触发，
// 回调里不能再调本对象的任何方法。

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "rudp_options.h"

struct IKCPCB;

namespace breeze
{

// 进程内的单调毫秒，32 位回绕（KCP 内部用差值比较，回绕无碍；约 49 天一圈）。
std::uint32_t KcpNowMs() noexcept;

class KcpSession
{
public:
    // output：KCP 要发一个段（含 24 字节头）时调用，data 只在回调内有效。
    using Output = void (*)(void* user, std::span<const std::byte> data);

    // 创建失败（内存不足、mtu 不合法）返回空。options 须已 Valid()。
    static std::unique_ptr<KcpSession> Create(std::uint32_t conv, const RudpOptions& options, Output out, void* user);
    ~KcpSession();

    KcpSession(const KcpSession&) = delete;
    KcpSession& operator=(const KcpSession&) = delete;

    // output 的 user 在创建时可能还不知道（会话对象要先于连接对象存在），之后补上。
    void SetUser(void* user) noexcept { user_ = user; }

    // 喂一个收到的段（Data 包去掉 1 字节包头之后的部分）。0 = 接受；负数 = conv 不符或格式不对，调用方丢弃。
    // 先把 KCP 的时钟拨到 now：不拨的话 RTT 要按上次 Update 的时刻算，白白多估一个 interval。
    int Input(std::span<const std::byte> segment, std::uint32_t now) noexcept;

    // 排一条消息，fragments 返回占了几段（≥ 1，空消息也占一段）。0 = 成功；负数 = 太大（超过 kMaxFragments 段）。
    // 消息保持边界，对端 Recv 一次得到整条。
    int Send(std::span<const std::byte> message, std::uint32_t& fragments) noexcept;

    // 下一条完整消息的长度；-1 = 还没有（分段没收齐）。
    int PeekSize() const noexcept;
    // 取走下一条消息到 out（须不小于 PeekSize()）。返回长度，负数 = 失败。
    int Recv(std::span<std::byte> out) noexcept;

    // 推进 KCP 的时钟：到点就 flush（发数据、重传、ack、窗口探测）。每个 interval 调一次。
    void Update(std::uint32_t now) noexcept;
    // 立刻 flush，不等 Update 的节拍。必须先 Update 过一次（ikcp 的要求）。
    void Flush() noexcept;

    // 已排队但还没被对端确认的段数（发送队列 + 在途）。
    std::uint32_t WaitSnd() const noexcept;
    // 现在调 Update 会不会做事：有没发完 / 没被确认的数据（队列或在途，意味着可能要重传）、有待发的确认、
    // 有待发的窗口通告 / 探测、或对端窗口为 0（要定时探测）。都没有就是静止的：Update 只会白走一遍，可以不调，
    // 直到下一次 Input / Send 打破静止（ikcp_update 对隔了很久的第一次调用会自己重置内部时钟）。
    bool NeedsUpdate() const noexcept;
    // 还在发送队列里、没进发送窗口的段数（不含已发出等确认的）。队列只在 flush 时往窗口里移，且先进先出。
    std::uint32_t QueuedSegments() const noexcept;
    // 同一个段重传次数超过 dead_link：链路判死。
    bool Dead() const noexcept;
    // 每段能装多少字节的消息（mtu - 包头 - KCP 头）。
    std::uint32_t mss() const noexcept;

private:
    KcpSession() = default;

    static int OutputThunk(const char* buf, int len, IKCPCB* kcp, void* user);

    IKCPCB* kcp_ = nullptr;
    Output out_ = nullptr;
    void* user_ = nullptr;
};

} // namespace breeze
