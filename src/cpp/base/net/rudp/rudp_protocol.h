// rudp 的数据报格式：握手、心跳、关闭是自己的控制包，数据包里装 KCP 段。设计见 docs/网络库设计.md §4.14。
//
// 每个数据报第一个字节是 0xA0 | 类型。整数一律小端（与 KCP 一致）。
//   Hello  [hdr][version u8][conv u32][token 16]   客户端 → 服务端：出示准入票据
//   Accept [hdr][version u8][conv u32]             服务端 → 客户端：会话建立
//   Reject [hdr][reason u8]                        服务端 → 客户端：拒绝（原因见 RejectReason）
//   Data   [hdr][KCP 段...]                        双向：KCP 自己的头里有 conv
//   Ping / Pong [hdr][conv u32]                    客户端心跳 / 服务端回应
//   Close  [hdr][conv u32][reason u8]              任一方：会话结束
//
// 服务端回应的控制包都不比触发它的包大（Reject 2 字节 < Hello 22 字节；Close 6 字节 < 最小的 Data 25 字节），
// 所以不能被拿来放大流量。Parse 是严格的：长度必须正好，多一个字节也丢（rudp_protocol_test）。

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace breeze::rudp
{

inline constexpr std::uint8_t kVersion = 1;
inline constexpr std::uint8_t kHeaderTag = 0xA0; // 高 4 位；低 4 位是 PacketType
inline constexpr std::size_t kTokenSize = 16;

// 与 KCP 绑定的常量，kcp_session.cpp 里 static_assert 它们与 ikcp.h 一致。
inline constexpr std::size_t kDataPrefix = 1;     // Data 包的 hdr
inline constexpr std::size_t kKcpHeader = 24;     // IKCP_OVERHEAD
inline constexpr std::size_t kMaxFragments = 127; // IKCP_WND_RCV - 1：一条消息最多这么多段（ikcp_send 超过返回 -2）

// mtu 是整个 UDP 载荷的上限（含 hdr 与 KCP 头）。下限是 IPv4 的最小重组缓冲，上限是以太网；巨帧不在目标内。
inline constexpr std::uint32_t kMinMtu = 576;
inline constexpr std::uint32_t kMaxMtu = 1500;

// 一条消息最大多少字节：kMaxFragments 段，每段 mss = mtu - hdr - KCP 头。
constexpr std::size_t MaxMessageForMtu(std::uint32_t mtu) noexcept
{
    return kMaxFragments * (mtu - kDataPrefix - kKcpHeader);
}

enum class PacketType : std::uint8_t
{
    Hello = 1,
    Accept,
    Reject,
    Data,
    Ping,
    Pong,
    Close,
};

enum class RejectReason : std::uint8_t
{
    Ticket = 1, // 票据不存在、已过期、已用过或 token 不对（不区分，免得做探测口）
    Busy,       // 连接表满
    Filtered,   // AcceptFilter 拒绝
    Version,    // 协议版本不支持
    Stopping,   // 服务端正在停止接受
};

using Token = std::array<std::byte, kTokenSize>;

struct Packet
{
    PacketType type = PacketType::Data;
    std::uint8_t version = 0;          // Hello / Accept
    std::uint32_t conv = 0;            // Hello / Accept / Ping / Pong / Close
    Token token{};                     // Hello
    std::uint8_t reason = 0;           // Reject（RejectReason）/ Close（CloseReason）
    std::span<const std::byte> body{}; // Data：KCP 段，指向入参的内存
};

// 解析一个数据报。不认识的类型、长度不对、Data 不够装一个 KCP 头，都返回空。
std::optional<Packet> Parse(std::span<const std::byte> datagram) noexcept;

// 控制包编码到定长缓冲里，view() 是有效部分。
struct Control
{
    std::array<std::byte, 24> bytes{};
    std::uint8_t size = 0;
    std::span<const std::byte> view() const noexcept { return {bytes.data(), size}; }
};

Control EncodeHello(std::uint32_t conv, const Token& token) noexcept;
Control EncodeAccept(std::uint32_t conv) noexcept;
Control EncodeReject(RejectReason reason) noexcept;
Control EncodePing(std::uint32_t conv) noexcept;
Control EncodePong(std::uint32_t conv) noexcept;
Control EncodeClose(std::uint32_t conv, std::uint8_t reason) noexcept;

// Data 包前缀（1 字节），与 KCP 段分两个 iovec 发出，不拷贝。
constexpr std::byte DataHeader() noexcept
{
    return static_cast<std::byte>(kHeaderTag | static_cast<std::uint8_t>(PacketType::Data));
}

// KCP 段头里的 conv（前 4 字节小端）；不够 4 字节返回 0。
std::uint32_t PeekKcpConv(std::span<const std::byte> kcp_segment) noexcept;

} // namespace breeze::rudp
