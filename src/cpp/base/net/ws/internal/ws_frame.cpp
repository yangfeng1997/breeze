#include "base/net/ws/internal/ws_frame.h"

#include <cstring>

namespace breeze::ws
{

HeaderParse ParseFrameHeader(std::span<const std::byte> data, bool expect_masked, FrameHeader& out) noexcept
{
    if (data.size() < 2)
        return HeaderParse::NeedMore;

    const auto b0 = static_cast<std::uint8_t>(data[0]);
    const auto b1 = static_cast<std::uint8_t>(data[1]);

    if ((b0 & 0x70) != 0)
        return HeaderParse::Error; // RSV1..3：没有协商过任何扩展
    const std::uint8_t op = b0 & 0x0F;
    if (!((op <= 0x2) || (op >= 0x8 && op <= 0xA)))
        return HeaderParse::Error; // 保留的 opcode
    const bool fin = (b0 & 0x80) != 0;
    const bool masked = (b1 & 0x80) != 0;
    if (masked != expect_masked)
        return HeaderParse::Error;

    std::uint64_t len = b1 & 0x7F;
    std::size_t pos = 2;
    if (len == 126)
    {
        if (data.size() < 4)
            return HeaderParse::NeedMore;
        len = (static_cast<std::uint64_t>(data[2]) << 8) | static_cast<std::uint64_t>(data[3]);
        if (len < 126)
            return HeaderParse::Error; // 非最短编码
        pos = 4;
    }
    else if (len == 127)
    {
        if (data.size() < 10)
            return HeaderParse::NeedMore;
        len = 0;
        for (std::size_t i = 0; i < 8; ++i)
            len = (len << 8) | static_cast<std::uint64_t>(data[2 + i]);
        if ((len >> 63) != 0 || len <= 0xFFFF)
            return HeaderParse::Error; // 最高位必须为 0；非最短编码
        pos = 10;
    }

    if (op >= 0x8 && (!fin || len > kMaxControlPayload))
        return HeaderParse::Error; // 控制帧不能分片、payload 不超过 125

    if (masked)
    {
        if (data.size() < pos + 4)
            return HeaderParse::NeedMore;
        std::memcpy(out.mask_key.data(), data.data() + pos, 4);
        pos += 4;
    }
    else
    {
        out.mask_key = {};
    }

    out.fin = fin;
    out.opcode = static_cast<Opcode>(op);
    out.masked = masked;
    out.payload_len = len;
    out.header_size = pos;
    return HeaderParse::Ok;
}

std::size_t EncodeFrameHeader(bool fin,
                              Opcode op,
                              std::uint64_t payload_len,
                              const std::array<std::byte, 4>* mask_key,
                              std::span<std::byte, kMaxHeaderSize> out) noexcept
{
    const std::uint8_t mask_bit = mask_key != nullptr ? 0x80 : 0x00;
    out[0] = static_cast<std::byte>((fin ? 0x80 : 0x00) | static_cast<std::uint8_t>(op));
    std::size_t pos;
    if (payload_len < 126)
    {
        out[1] = static_cast<std::byte>(mask_bit | payload_len);
        pos = 2;
    }
    else if (payload_len <= 0xFFFF)
    {
        out[1] = static_cast<std::byte>(mask_bit | 126);
        out[2] = static_cast<std::byte>(payload_len >> 8);
        out[3] = static_cast<std::byte>(payload_len);
        pos = 4;
    }
    else
    {
        out[1] = static_cast<std::byte>(mask_bit | 127);
        for (std::size_t i = 0; i < 8; ++i)
            out[2 + i] = static_cast<std::byte>(payload_len >> (8 * (7 - i)));
        pos = 10;
    }
    if (mask_key != nullptr)
    {
        std::memcpy(out.data() + pos, mask_key->data(), 4);
        pos += 4;
    }
    return pos;
}

void ApplyMask(std::span<std::byte> data, const std::array<std::byte, 4>& key) noexcept
{
    // 8 字节一组：把键重复两遍装进一个 64 位数。XOR 是逐字节的，用 memcpy 搬运不受字节序影响。
    std::byte key8[8];
    std::memcpy(key8, key.data(), 4);
    std::memcpy(key8 + 4, key.data(), 4);
    std::uint64_t k64;
    std::memcpy(&k64, key8, 8);

    std::byte* p = data.data();
    std::size_t n = data.size();
    for (; n >= 8; p += 8, n -= 8)
    {
        std::uint64_t v;
        std::memcpy(&v, p, 8);
        v ^= k64;
        std::memcpy(p, &v, 8);
    }
    for (std::size_t i = 0; i < n; ++i) // 起点是 8 的倍数，所以键从头开始
        p[i] ^= key[i & 3];
}

} // namespace breeze::ws
