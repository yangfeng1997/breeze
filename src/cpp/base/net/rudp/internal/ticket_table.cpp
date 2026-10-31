#include "ticket_table.h"

#include <sys/random.h>

#include <cerrno>
#include <cstring>

namespace breeze
{

namespace
{

// 系统随机数。getrandom 在阻塞模式下对 ≤256 字节不会被信号打断也不会短读（man 2 getrandom），
// 这里仍处理 EINTR 与短读：宁可多一个循环，也不能把没填满的缓冲当 token 用。
bool FillRandom(void* buf, std::size_t n)
{
    auto* p = static_cast<std::byte*>(buf);
    while (n > 0)
    {
        const ssize_t r = ::getrandom(p, n, 0);
        if (r < 0)
        {
            if (errno == EINTR)
                continue;
            return false;
        }
        p += r;
        n -= static_cast<std::size_t>(r);
    }
    return true;
}

// 常量时间比较：不因首个不同字节的位置提前返回。
bool TokenEqual(const rudp::Token& a, const rudp::Token& b)
{
    unsigned diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned>(a[i] ^ b[i]);
    return diff == 0;
}

} // namespace

void TicketTable::SweepLocked(Clock::time_point now)
{
    std::erase_if(entries_, [now](const auto& kv) { return kv.second.expires <= now; });
    next_sweep_ = now + std::chrono::seconds(1);
}

bool TicketTable::InsertLocked(std::uint32_t conv,
                               const rudp::Token& token,
                               std::chrono::milliseconds ttl,
                               Clock::time_point now)
{
    if (conv == 0 || ttl.count() <= 0)
        return false;
    if (now >= next_sweep_ || entries_.size() >= max_pending_)
        SweepLocked(now);
    if (entries_.size() >= max_pending_)
        return false;
    // 已有未过期的同 conv：拒绝（覆盖会让先拿到票据的客户端莫名其妙握手失败）。过期的已被上面的清扫删掉，
    // 但清扫可能因为 next_sweep_ 没到而跳过，所以这里再判一次。
    if (const auto it = entries_.find(conv); it != entries_.end())
    {
        if (it->second.expires > now)
            return false;
        entries_.erase(it);
    }
    entries_.emplace(conv, Entry{token, now + ttl});
    return true;
}

std::optional<RudpTicket> TicketTable::Issue(std::chrono::milliseconds ttl, Clock::time_point now)
{
    RudpTicket t;
    const std::lock_guard lk(mu_);
    // conv 撞上已有的概率是表大小 / 2^32，重试几次足够；还撞就放弃，不死循环。
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        if (!FillRandom(&t.conv, sizeof(t.conv)) || !FillRandom(t.token.data(), t.token.size()))
            return std::nullopt;
        if (t.conv == 0)
            continue;
        if (InsertLocked(t.conv, t.token, ttl, now))
            return t;
        if (ttl.count() <= 0 || entries_.size() >= max_pending_)
            return std::nullopt; // 重试也没用
    }
    return std::nullopt;
}

bool TicketTable::Add(const RudpTicket& ticket, std::chrono::milliseconds ttl, Clock::time_point now)
{
    const std::lock_guard lk(mu_);
    return InsertLocked(ticket.conv, ticket.token, ttl, now);
}

bool TicketTable::Consume(std::uint32_t conv, const rudp::Token& token, Clock::time_point now)
{
    const std::lock_guard lk(mu_);
    const auto it = entries_.find(conv);
    if (it == entries_.end())
        return false;
    if (it->second.expires <= now)
    {
        entries_.erase(it);
        return false;
    }
    if (!TokenEqual(it->second.token, token))
        return false; // 不删：见头注释
    entries_.erase(it);
    return true;
}

std::size_t TicketTable::size() const
{
    const std::lock_guard lk(mu_);
    return entries_.size();
}

} // namespace breeze
