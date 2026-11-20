// base64（RFC 4648 §4，标准字母表，带 '=' 填充）。给 WebSocket 握手用：Sec-WebSocket-Key / Sec-WebSocket-Accept。
// 解码是**严格**的（握手头来自不可信的对端）：长度必须是 4 的倍数，只认标准字母表与末尾的 '='，不接受空白、换行、
// URL 安全字母表，也不接受填充位不为零的"非规范"编码（同一份字节只有一种合法写法）。
//
// 只有头文件。

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace breeze::str
{

inline std::string Base64Encode(std::span<const std::byte> in)
{
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 3 <= in.size(); i += 3)
    {
        const std::uint32_t v = (static_cast<std::uint32_t>(in[i]) << 16) | (static_cast<std::uint32_t>(in[i + 1]) << 8) |
                                static_cast<std::uint32_t>(in[i + 2]);
        out.push_back(kAlphabet[(v >> 18) & 63]);
        out.push_back(kAlphabet[(v >> 12) & 63]);
        out.push_back(kAlphabet[(v >> 6) & 63]);
        out.push_back(kAlphabet[v & 63]);
    }
    const std::size_t rest = in.size() - i;
    if (rest == 1)
    {
        const std::uint32_t v = static_cast<std::uint32_t>(in[i]) << 16;
        out.push_back(kAlphabet[(v >> 18) & 63]);
        out.push_back(kAlphabet[(v >> 12) & 63]);
        out += "==";
    }
    else if (rest == 2)
    {
        const std::uint32_t v = (static_cast<std::uint32_t>(in[i]) << 16) | (static_cast<std::uint32_t>(in[i + 1]) << 8);
        out.push_back(kAlphabet[(v >> 18) & 63]);
        out.push_back(kAlphabet[(v >> 12) & 63]);
        out.push_back(kAlphabet[(v >> 6) & 63]);
        out.push_back('=');
    }
    return out;
}

inline std::string Base64Encode(std::string_view in) { return Base64Encode(std::as_bytes(std::span(in))); }

// 不合法（长度、字符、填充、非规范编码）返回 nullopt。
inline std::optional<std::vector<std::byte>> Base64Decode(std::string_view in)
{
    const auto value = [](char c) -> int
    {
        if (c >= 'A' && c <= 'Z')
            return c - 'A';
        if (c >= 'a' && c <= 'z')
            return c - 'a' + 26;
        if (c >= '0' && c <= '9')
            return c - '0' + 52;
        if (c == '+')
            return 62;
        if (c == '/')
            return 63;
        return -1;
    };
    if (in.size() % 4 != 0)
        return std::nullopt;
    std::vector<std::byte> out;
    out.reserve(in.size() / 4 * 3);
    for (std::size_t i = 0; i < in.size(); i += 4)
    {
        const bool last = i + 4 == in.size();
        const int a = value(in[i]);
        const int b = value(in[i + 1]);
        if (a < 0 || b < 0)
            return std::nullopt;
        if (in[i + 2] == '=')
        {
            // "xx==": 只能出现在末尾，且第二个字符的低 4 位必须为 0（否则同一份字节有多种写法）
            if (!last || in[i + 3] != '=' || (b & 15) != 0)
                return std::nullopt;
            out.push_back(static_cast<std::byte>((a << 2) | (b >> 4)));
            continue;
        }
        const int c = value(in[i + 2]);
        if (c < 0)
            return std::nullopt;
        if (in[i + 3] == '=')
        {
            if (!last || (c & 3) != 0)
                return std::nullopt;
            out.push_back(static_cast<std::byte>((a << 2) | (b >> 4)));
            out.push_back(static_cast<std::byte>(((b & 15) << 4) | (c >> 2)));
            continue;
        }
        const int d = value(in[i + 3]);
        if (d < 0)
            return std::nullopt;
        out.push_back(static_cast<std::byte>((a << 2) | (b >> 4)));
        out.push_back(static_cast<std::byte>(((b & 15) << 4) | (c >> 2)));
        out.push_back(static_cast<std::byte>(((c & 3) << 6) | d));
    }
    return out;
}

} // namespace breeze::str
