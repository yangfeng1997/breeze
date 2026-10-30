// 字符串工具：分割 / 拼接 / 修剪 / ASCII 大小写 / 替换 / 数字解析 / 格式化。
//
// 约定：
// - 输入一律 string_view，不拷贝；返回 string_view 的函数（Split / SplitAny / Trim*）结果指向入参的内存，
//   入参活着它才有效。传入临时 std::string 会编译失败（见各函数上的 = delete），要拷贝用 SplitOwned。
// - 大小写只认 ASCII，与 locale 无关（std::toupper 受 setlocale 影响，在 tr_TR 这类 locale 下 'i' 会变成别的字符）。
//   UTF-8 多字节序列的字节都 >= 0x80，原样保留。
// - 数字解析是严格的：整串必须恰好是一个数，不跳空白、不认前导 '+'（同 std::from_chars），要宽松先 Trim。
//   失败返回 nullopt，不抛异常；浮点的 inf / nan 一律视为失败（配置里出现它们几乎一定是笔误）。
// - 已有标准库等价物的不重复提供：starts_with / ends_with / contains 用 std::string_view 的成员，格式化用 std::format
//   （容器、tuple 也能直接 {} 打印）。

#pragma once

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace breeze::str
{

// 万能引用推导出 S == std::string（不是 std::string&）说明实参是临时对象。下面用它禁掉"返回视图却传了临时串"。
// 不能直接写 std::string&& 重载：字面量既能转 string_view 也能转 std::string&&，两个都是用户转换，调用会有歧义。
template <typename S>
concept TemporaryString = std::is_same_v<S, std::string>;

// ---- 分割 ----

// 按单个字符分割。"" 分割得到一个空段（skip_empty 时得到零段）；"a,,b" 得到 a、空、b。
// Join(Split(s, c), string(1, c)) == s 在 skip_empty = false 时恒成立。
std::vector<std::string_view> Split(std::string_view s, char delim, bool skip_empty = false);
// 按子串分割。delim 为空时不分割，整串作为唯一一段。
std::vector<std::string_view> Split(std::string_view s, std::string_view delim, bool skip_empty = false);
// 按 delims 中的任意一个字符分割（如 " ,;"）。
std::vector<std::string_view> SplitAny(std::string_view s, std::string_view delims, bool skip_empty = false);

template <TemporaryString S>
std::vector<std::string_view> Split(S&&, char, bool = false) = delete;
template <TemporaryString S>
std::vector<std::string_view> Split(S&&, std::string_view, bool = false) = delete;
template <TemporaryString S>
std::vector<std::string_view> SplitAny(S&&, std::string_view, bool = false) = delete;

// 需要拥有各段（原串会先销毁）时用。
std::vector<std::string> SplitOwned(std::string_view s, char delim, bool skip_empty = false);

// ---- 拼接 ----

// 元素可以是任何能转成 string_view 的东西（std::string / string_view / const char*）。
template <std::ranges::input_range R>
    requires std::convertible_to<std::ranges::range_reference_t<R>, std::string_view>
std::string Join(R&& parts, std::string_view sep)
{
    std::string out;
    bool first = true;
    for (auto&& p : parts)
    {
        if (!first)
            out.append(sep);
        first = false;
        out.append(std::string_view(p));
    }
    return out;
}

inline std::string Join(std::initializer_list<std::string_view> parts, std::string_view sep)
{
    return Join(std::vector<std::string_view>(parts), sep);
}

// ---- 修剪 ----

// 默认修剪的空白：空格 \t \r \n \v \f。
inline constexpr std::string_view kWhitespace = " \t\r\n\v\f";

std::string_view TrimLeft(std::string_view s, std::string_view chars = kWhitespace);
std::string_view TrimRight(std::string_view s, std::string_view chars = kWhitespace);
std::string_view Trim(std::string_view s, std::string_view chars = kWhitespace);

template <TemporaryString S>
std::string_view TrimLeft(S&&, std::string_view = kWhitespace) = delete;
template <TemporaryString S>
std::string_view TrimRight(S&&, std::string_view = kWhitespace) = delete;
template <TemporaryString S>
std::string_view Trim(S&&, std::string_view = kWhitespace) = delete;

// ---- ASCII 大小写 ----

std::string ToUpper(std::string_view s);
std::string ToLower(std::string_view s);
void ToUpperInPlace(std::string& s) noexcept;
void ToLowerInPlace(std::string& s) noexcept;
bool EqualsIgnoreCase(std::string_view a, std::string_view b) noexcept;

// ---- 替换 / 重复 ----

// 把 s 里的 from 替换成 to，最多 max_count 处，从左到右不重叠；替换结果不会再被扫描（to 里含 from 也不会死循环）。
// from 为空时原样返回。
std::string Replace(std::string_view s,
                    std::string_view from,
                    std::string_view to,
                    std::size_t max_count = std::string_view::npos);
inline std::string ReplaceAll(std::string_view s, std::string_view from, std::string_view to)
{
    return Replace(s, from, to);
}

// 重复 n 次；n 为 0 或 s 为空返回空串。
std::string Repeat(std::string_view s, std::size_t n);

// ---- 数字解析 ----

// 整数 / 浮点（不含 bool、char）。整数可指定进制 2..36；超出 T 的范围、有多余字符、空串都失败。
template <typename T>
    requires(std::is_arithmetic_v<T> && !std::is_same_v<std::remove_cv_t<T>, bool> &&
             !std::is_same_v<std::remove_cv_t<T>, char>)
std::optional<T> ParseNumber(std::string_view s, int base = 10)
{
    T v{};
    const char* const first = s.data();
    const char* const last = s.data() + s.size();
    std::from_chars_result r;
    if constexpr (std::is_integral_v<T>)
        r = std::from_chars(first, last, v, base);
    else
        r = std::from_chars(first, last, v);
    if (r.ec != std::errc{} || r.ptr != last)
        return std::nullopt;
    if constexpr (std::is_floating_point_v<T>)
    {
        if (!std::isfinite(v))
            return std::nullopt;
    }
    return v;
}

// 解析失败给默认值。
template <typename T>
T ToNumber(std::string_view s, T default_value = T{})
{
    return ParseNumber<T>(s).value_or(default_value);
}

// true/false、yes/no、on/off、1/0，ASCII 不分大小写。
std::optional<bool> ParseBool(std::string_view s) noexcept;

// ---- 格式化 ----

// 千分位："1234567" -> "1,234,567"，负数带 '-'，int64 最小值也正确。
std::string FormatWithCommas(std::int64_t value);

} // namespace breeze::str
