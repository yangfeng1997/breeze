// UTF-8 校验（RFC 3629）：拒绝过长编码（C0 / C1、E0 80..9F、F0 80..8F）、UTF-16 代理对（U+D800..DFFF）、
// 超过 U+10FFFF 的码点（F4 90.. 与 F5..FF）。接受 U+0000（NUL 是合法码点）。
//
// 增量：Feed 可以分多段喂（WebSocket 分片的文本消息按帧到达，要边到边校验、尽早失败），AtBoundary 检查数据结束时
// 没有停在多字节字符中间。校验器只有 3 个字节的状态，可以直接放进每连接的对象里。
//
// 只有头文件，只依赖标准库。

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace breeze::str
{

class Utf8Validator
{
public:
    // 喂入一段。返回 false = 已确定非法（此后状态未定义，调用方应当丢弃整条数据）。
    bool Feed(std::span<const std::byte> in) noexcept
    {
        const auto* p = reinterpret_cast<const std::uint8_t*>(in.data());
        const std::uint8_t* const end = p + in.size();
        while (p != end)
        {
            if (need_ == 0)
            {
                // 快路径：纯 ASCII 一次 8 字节。文本消息绝大多数是 ASCII（JSON），这一段决定吞吐。
                while (end - p >= 8)
                {
                    std::uint64_t v;
                    std::memcpy(&v, p, 8);
                    if ((v & 0x8080808080808080ull) != 0)
                        break;
                    p += 8;
                }
                if (p == end)
                    break;
                const std::uint8_t b = *p++;
                if (b < 0x80)
                    continue;
                if (b < 0xC2)
                    return false; // 0x80..0xBF 是孤立的后续字节；0xC0 / 0xC1 只能编码 U+0000..007F（过长）
                if (b < 0xE0)
                {
                    need_ = 1;
                }
                else if (b < 0xF0)
                {
                    need_ = 2;
                    lo_ = (b == 0xE0) ? 0xA0 : 0x80; // E0 80..9F 过长
                    hi_ = (b == 0xED) ? 0x9F : 0xBF; // ED A0..BF 是代理对
                }
                else if (b < 0xF5)
                {
                    need_ = 3;
                    lo_ = (b == 0xF0) ? 0x90 : 0x80; // F0 80..8F 过长
                    hi_ = (b == 0xF4) ? 0x8F : 0xBF; // F4 90.. 超过 U+10FFFF
                }
                else
                {
                    return false;
                }
            }
            else
            {
                const std::uint8_t b = *p++;
                if (b < lo_ || b > hi_)
                    return false;
                lo_ = 0x80; // 只有第一个后续字节有特殊范围
                hi_ = 0xBF;
                --need_;
            }
        }
        return true;
    }
    bool Feed(std::string_view s) noexcept { return Feed(std::as_bytes(std::span(s))); }

    // 数据结束：没有停在多字节字符中间。
    bool AtBoundary() const noexcept { return need_ == 0; }

    void Reset() noexcept
    {
        need_ = 0;
        lo_ = 0x80;
        hi_ = 0xBF;
    }

private:
    std::uint8_t need_ = 0; // 还差几个后续字节
    std::uint8_t lo_ = 0x80; // 下一个后续字节的合法范围
    std::uint8_t hi_ = 0xBF;
};

// 一次性校验整段。
inline bool IsValidUtf8(std::span<const std::byte> in) noexcept
{
    Utf8Validator v;
    return v.Feed(in) && v.AtBoundary();
}
inline bool IsValidUtf8(std::string_view s) noexcept { return IsValidUtf8(std::as_bytes(std::span(s))); }

} // namespace breeze::str
