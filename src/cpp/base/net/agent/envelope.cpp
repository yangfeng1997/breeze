#include "base/net/agent/envelope.h"

#include <cstring>

namespace breeze::agent
{

EnvStatus ParseEnvelope(std::span<const std::byte> bytes, EnvelopeHead& out) noexcept
{
    if (bytes.size() < kEnvelopeHeadSize)
        return EnvStatus::kTooShort;
    std::memcpy(&out, bytes.data(), kEnvelopeHeadSize); // 先拷到本地再校验
    if (out.magic != kEnvelopeMagic)
        return EnvStatus::kBadMagic;
    if (out.version != kEnvelopeVersion)
        return EnvStatus::kBadVersion;
    if (out.type < static_cast<std::uint8_t>(EnvType::kRequest) || out.type > static_cast<std::uint8_t>(EnvType::kOneway))
        return EnvStatus::kBadType;
    if ((out.flags & ~kEnvKnownFlags) != 0)
        return EnvStatus::kBadFlags;
    if (out.reserved0[0] != 0 || out.reserved0[1] != 0 || out.reserved0[2] != 0)
        return EnvStatus::kBadReserved;
    if (out.route_type < static_cast<std::uint8_t>(RouteType::kDirect) ||
        out.route_type > static_cast<std::uint8_t>(RouteType::kBroadcast))
        return EnvStatus::kBadRoute;
    const bool direct = out.route_type == static_cast<std::uint8_t>(RouteType::kDirect);
    if (direct && out.dst == 0)
        return EnvStatus::kBadRoute;
    // 回包只能点对点地回给请求方。
    if (out.type == static_cast<std::uint8_t>(EnvType::kResponse) && !direct)
        return EnvStatus::kBadRoute;
    // 先比较再相减的写法：bytes.size() 已经 >= 64。
    if (static_cast<std::uint64_t>(out.body_len) != bytes.size() - kEnvelopeHeadSize)
        return EnvStatus::kBadLength;
    return EnvStatus::kOk;
}

void WriteEnvelopeHead(std::span<std::byte> dst, const EnvelopeHead& head) noexcept
{
    std::memcpy(dst.data(), &head, kEnvelopeHeadSize);
}

ExpireStatus CheckExpire(std::uint32_t remaining_ms,
                         std::uint64_t enqueue_ts_ns,
                         std::uint64_t now_ns,
                         std::uint32_t* left_ms) noexcept
{
    if (remaining_ms == 0 || enqueue_ts_ns == 0)
        return ExpireStatus::kInvalid;
    const std::uint64_t age_ns = now_ns > enqueue_ts_ns ? now_ns - enqueue_ts_ns : 0;
    const std::uint64_t budget_ns = static_cast<std::uint64_t>(remaining_ms) * 1'000'000;
    if (age_ns >= budget_ns)
        return ExpireStatus::kExpired;
    // 对剩余向下取整：不足 1 毫秒的零头不要（写进去的 remaining_ms 不能是 0），宁可早丢。
    const std::uint64_t left = (budget_ns - age_ns) / 1'000'000;
    if (left == 0)
        return ExpireStatus::kExpired;
    if (left_ms != nullptr)
        *left_ms = static_cast<std::uint32_t>(left);
    return ExpireStatus::kAlive;
}

bool PatchForForward(EnvelopeHead& head, std::uint32_t left_ms) noexcept
{
    if (head.hops >= kMaxHops)
        return false;
    ++head.hops;
    head.remaining_ms = left_ms;
    return true;
}

} // namespace breeze::agent
