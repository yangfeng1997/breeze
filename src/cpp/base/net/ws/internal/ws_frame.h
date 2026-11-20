// WebSocket 帧（RFC 6455 §5）：帧头的解析 / 编码、掩码、关闭码。纯函数，不碰连接与缓冲，单独可测。
// 属于 ws 的内部实现，不是公开头。

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace breeze::ws
{

enum class Opcode : std::uint8_t
{
    Continuation = 0x0,
    Text = 0x1,
    Binary = 0x2,
    Close = 0x8,
    Ping = 0x9,
    Pong = 0xA,
};

constexpr bool IsControl(Opcode op) noexcept { return static_cast<std::uint8_t>(op) >= 0x8; }

inline constexpr std::size_t kMaxControlPayload = 125;
// 2 字节基本头 + 8 字节扩展长度 + 4 字节掩码键。不超过 IFrameCodec 的 kMaxFrameHeaderSize（16）。
inline constexpr std::size_t kMaxHeaderSize = 14;

struct FrameHeader
{
    bool fin = false;
    Opcode opcode = Opcode::Continuation;
    bool masked = false;
    std::array<std::byte, 4> mask_key{};
    std::uint64_t payload_len = 0;
    std::size_t header_size = 0; // 帧头占的字节数（含掩码键）
};

enum class HeaderParse : std::uint8_t
{
    NeedMore, // 帧头还不完整
    Ok,
    Error, // 协议错误：关闭码 1002
};

// 解析帧头。只做语法检查，与长度上限无关（上限由调用方在拿到 payload_len 之后判，先于等待 payload）。
// expect_masked：这一方向上帧的 MASK 位必须是什么（服务端收客户端帧 = true，客户端收服务端帧 = false）。
// Error 的情形：RSV 位非零（没有协商任何扩展）、保留的 opcode、控制帧分片或超过 125 字节、长度没有用最短编码
// （126 形式小于 126、127 形式不超过 65535）、64 位长度的最高位为 1、MASK 位与方向不符。
HeaderParse ParseFrameHeader(std::span<const std::byte> data, bool expect_masked, FrameHeader& out) noexcept;

// 写帧头，返回字节数（2 / 4 / 10，带掩码键时各加 4）。mask_key 非空时置 MASK 位并写入这 4 个字节；
// payload 的掩码由调用方用 ApplyMask 做。
std::size_t EncodeFrameHeader(bool fin,
                              Opcode op,
                              std::uint64_t payload_len,
                              const std::array<std::byte, 4>* mask_key,
                              std::span<std::byte, kMaxHeaderSize> out) noexcept;

// data[i] ^= key[i % 4]，原地。一次 8 字节，编译器会向量化。再调一次就是去掩码。
void ApplyMask(std::span<std::byte> data, const std::array<std::byte, 4>& key) noexcept;

// 线上允许出现的关闭码（RFC 6455 §7.4）：1000–1003、1007–1011、3000–4999。
// 1004 保留，1005 / 1006 / 1015 只在本端内部使用，不得出现在 Close 帧里。
constexpr bool IsValidCloseCode(std::uint16_t code) noexcept
{
    return (code >= 1000 && code <= 1003) || (code >= 1007 && code <= 1011) || (code >= 3000 && code <= 4999);
}

// 没有状态码的 Close 帧在本端表现的关闭码（RFC 6455 §7.1.5）。
inline constexpr std::uint16_t kCloseNoStatus = 1005;

} // namespace breeze::ws
