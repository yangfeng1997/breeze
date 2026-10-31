// base/shm 测试的公共件。无测试框架依赖：失败即 abort；用 Arm 设一个闹钟，卡死也会终止进程而不是挂住 ctest。

#pragma once

#include <sys/mman.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>

#include "shm_channel.h"

#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                              \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (0)

namespace breeze::shm::testing
{

// 卡死保护：seconds 秒之后 SIGALRM 终止进程（fork 出来的子进程不继承闹钟，子进程自己再设）。
inline void Arm(unsigned seconds) { ::alarm(seconds); }

// 载荷：前 8 字节是序号，其余按序号生成的确定性字节。校验时整条核对，半条记录或串位会立刻暴露。
inline void FillPayload(std::span<std::byte> p, std::uint64_t seq)
{
    CHECK(p.size() >= 8);
    std::memcpy(p.data(), &seq, 8);
    for (std::size_t i = 8; i < p.size(); ++i)
        p[i] = static_cast<std::byte>((seq * 131 + i * 7 + (seq >> 8)) & 0xFF);
}

// 载荷合法返回 true，并取出序号。
inline bool CheckPayload(const std::byte* p, std::uint32_t len, std::uint64_t* seq_out)
{
    if (len < 8)
        return false;
    std::uint64_t seq;
    std::memcpy(&seq, p, 8);
    for (std::uint32_t i = 8; i < len; ++i)
    {
        if (p[i] != static_cast<std::byte>((seq * 131 + i * 7 + (seq >> 8)) & 0xFF))
            return false;
    }
    *seq_out = seq;
    return true;
}

// 用 1 MiB 的小环建通道，测试跑得快；容量相关的用例自己传选项。
inline ShmChannel MakeChannel(std::uint64_t tx_cap = kMinRingCapacity, std::uint64_t rx_cap = kMinRingCapacity)
{
    ChannelOptions o;
    o.tx_capacity = tx_cap;
    o.rx_capacity = rx_cap;
    auto ch = ShmChannel::Create(o);
    CHECK(ch.has_value());
    return std::move(*ch);
}

// 复制三个 fd，交给 Attach 去接手（模拟经 UDS 传过来的 fd）。
inline std::expected<ShmChannel, int> AttachDup(const ShmChannel& ch, const AttachLimits& limits = {})
{
    return ShmChannel::Attach(Fd(::dup(ch.memfd())), Fd(::dup(ch.tx_eventfd())), Fd(::dup(ch.rx_eventfd())), limits);
}

} // namespace breeze::shm::testing
