// 服务端的待用票据表：控制面线程签发 / 登记，IO 线程在握手时一次性取走。线程安全（一把互斥锁，握手不是热路径）。
// 设计见 docs/网络库设计.md §4.14。
//
// - 一次性：Consume 成功即删除，同一票据第二次出示失败（重放）。
// - token 错的不删：否则知道 conv 的人用错 token 试一次就能作废合法用户的票据。token 16 字节随机，在线猜不现实。
// - 过期由调用方给的 now 判定；过期项在 Consume 时顺手删，表满或每秒一次在登记时整表清扫。
// - token 比较是常量时间的（不因首个不同字节的位置而提前返回）。

#pragma once

#include <chrono>
#include <cstddef>
#include <mutex>
#include <optional>
#include <unordered_map>

#include "base/net/rudp/rudp_protocol.h"
#include "base/net/rudp/rudp_ticket.h"

namespace breeze
{

class TicketTable
{
public:
    using Clock = std::chrono::steady_clock;

    explicit TicketTable(std::size_t max_pending)
        : max_pending_(max_pending)
    {
    }

    // 生成随机 conv（非 0、表里没有）与 token 并登记。ttl ≤ 0 或表满（清扫过期项之后仍满）返回空。
    std::optional<RUdpTicket> Issue(std::chrono::milliseconds ttl, Clock::time_point now = Clock::now());

    // 登记别处（比如 login 进程）签发的票据。conv 为 0、ttl ≤ 0、conv 已有未过期的、表满返回 false。
    bool Add(const RUdpTicket& ticket, std::chrono::milliseconds ttl, Clock::time_point now = Clock::now());

    // conv 与 token 都对、且未过期：取走并返回 true。其余返回 false（原因不区分）。
    bool Consume(std::uint32_t conv, const rudp::Token& token, Clock::time_point now = Clock::now());

    // 当前登记的数量，含已过期但还没被清扫的。
    std::size_t size() const;

private:
    struct Entry
    {
        rudp::Token token;
        Clock::time_point expires;
    };

    bool InsertLocked(std::uint32_t conv,
                      const rudp::Token& token,
                      std::chrono::milliseconds ttl,
                      Clock::time_point now);
    void SweepLocked(Clock::time_point now);

    const std::size_t max_pending_;
    mutable std::mutex mu_;
    std::unordered_map<std::uint32_t, Entry> entries_;
    Clock::time_point next_sweep_{};
};

} // namespace breeze
