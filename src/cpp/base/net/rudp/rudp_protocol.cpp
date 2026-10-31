#include "base/net/rudp/rudp_protocol.h"

#include <algorithm>

namespace breeze::rudp
{

namespace
{

constexpr std::size_t kHelloSize = 1 + 1 + 4 + kTokenSize;
constexpr std::size_t kAcceptSize = 1 + 1 + 4;
constexpr std::size_t kRejectSize = 1 + 1;
constexpr std::size_t kPingSize = 1 + 4; // Ping / Pong
constexpr std::size_t kCloseSize = 1 + 4 + 1;

// 控制包必须装得进 Control，且都比最小的 Data 包（hdr + KCP 头）小：服务端回应不放大。
static_assert(kHelloSize <= sizeof(Control::bytes));
static_assert(kCloseSize < kDataPrefix + kKcpHeader);
static_assert(kRejectSize < kHelloSize && kAcceptSize <= kHelloSize);

std::uint32_t ReadLe32(const std::byte* p) noexcept
{
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
           static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}

void WriteLe32(std::byte* p, std::uint32_t v) noexcept
{
    p[0] = static_cast<std::byte>(v);
    p[1] = static_cast<std::byte>(v >> 8);
    p[2] = static_cast<std::byte>(v >> 16);
    p[3] = static_cast<std::byte>(v >> 24);
}

std::byte Header(PacketType t) noexcept { return static_cast<std::byte>(kHeaderTag | static_cast<std::uint8_t>(t)); }

} // namespace

std::optional<Packet> Parse(std::span<const std::byte> d) noexcept
{
    if (d.empty())
        return std::nullopt;
    const auto h = static_cast<std::uint8_t>(d[0]);
    if ((h & 0xF0) != kHeaderTag)
        return std::nullopt;
    Packet p;
    p.type = static_cast<PacketType>(h & 0x0F);
    switch (p.type)
    {
        case PacketType::Hello:
            if (d.size() != kHelloSize)
                return std::nullopt;
            p.version = static_cast<std::uint8_t>(d[1]);
            p.conv = ReadLe32(&d[2]);
            std::copy_n(d.begin() + 6, kTokenSize, p.token.begin());
            return p;
        case PacketType::Accept:
            if (d.size() != kAcceptSize)
                return std::nullopt;
            p.version = static_cast<std::uint8_t>(d[1]);
            p.conv = ReadLe32(&d[2]);
            return p;
        case PacketType::Reject:
            if (d.size() != kRejectSize)
                return std::nullopt;
            p.reason = static_cast<std::uint8_t>(d[1]);
            return p;
        case PacketType::Data:
            if (d.size() < kDataPrefix + kKcpHeader)
                return std::nullopt;
            p.body = d.subspan(kDataPrefix);
            return p;
        case PacketType::Ping:
        case PacketType::Pong:
            if (d.size() != kPingSize)
                return std::nullopt;
            p.conv = ReadLe32(&d[1]);
            return p;
        case PacketType::Close:
            if (d.size() != kCloseSize)
                return std::nullopt;
            p.conv = ReadLe32(&d[1]);
            p.reason = static_cast<std::uint8_t>(d[5]);
            return p;
    }
    return std::nullopt; // 类型 0 与 8 以上
}

Control EncodeHello(std::uint32_t conv, const Token& token) noexcept
{
    Control c;
    c.bytes[0] = Header(PacketType::Hello);
    c.bytes[1] = static_cast<std::byte>(kVersion);
    WriteLe32(&c.bytes[2], conv);
    std::copy(token.begin(), token.end(), c.bytes.begin() + 6);
    c.size = kHelloSize;
    return c;
}

Control EncodeAccept(std::uint32_t conv) noexcept
{
    Control c;
    c.bytes[0] = Header(PacketType::Accept);
    c.bytes[1] = static_cast<std::byte>(kVersion);
    WriteLe32(&c.bytes[2], conv);
    c.size = kAcceptSize;
    return c;
}

Control EncodeReject(RejectReason reason) noexcept
{
    Control c;
    c.bytes[0] = Header(PacketType::Reject);
    c.bytes[1] = static_cast<std::byte>(reason);
    c.size = kRejectSize;
    return c;
}

Control EncodePing(std::uint32_t conv) noexcept
{
    Control c;
    c.bytes[0] = Header(PacketType::Ping);
    WriteLe32(&c.bytes[1], conv);
    c.size = kPingSize;
    return c;
}

Control EncodePong(std::uint32_t conv) noexcept
{
    Control c = EncodePing(conv);
    c.bytes[0] = Header(PacketType::Pong);
    return c;
}

Control EncodeClose(std::uint32_t conv, std::uint8_t reason) noexcept
{
    Control c;
    c.bytes[0] = Header(PacketType::Close);
    WriteLe32(&c.bytes[1], conv);
    c.bytes[5] = static_cast<std::byte>(reason);
    c.size = kCloseSize;
    return c;
}

std::uint32_t PeekKcpConv(std::span<const std::byte> seg) noexcept { return seg.size() < 4 ? 0 : ReadLe32(seg.data()); }

} // namespace breeze::rudp
