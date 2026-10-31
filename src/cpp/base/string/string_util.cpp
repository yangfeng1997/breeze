#include "base/string/string_util.h"

#include <algorithm>

namespace breeze::str
{

namespace
{

constexpr char UpperAscii(char c) noexcept { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }
constexpr char LowerAscii(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

} // namespace

std::vector<std::string_view> Split(std::string_view s, char delim, bool skip_empty)
{
    std::vector<std::string_view> out;
    std::size_t start = 0;
    for (;;)
    {
        const std::size_t pos = s.find(delim, start);
        const std::string_view seg = s.substr(start, pos == std::string_view::npos ? pos : pos - start);
        if (!skip_empty || !seg.empty())
            out.push_back(seg);
        if (pos == std::string_view::npos)
            return out;
        start = pos + 1;
    }
}

std::vector<std::string_view> Split(std::string_view s, std::string_view delim, bool skip_empty)
{
    if (delim.empty())
    {
        if (skip_empty && s.empty())
            return {};
        return {s};
    }
    std::vector<std::string_view> out;
    std::size_t start = 0;
    for (;;)
    {
        const std::size_t pos = s.find(delim, start);
        const std::string_view seg = s.substr(start, pos == std::string_view::npos ? pos : pos - start);
        if (!skip_empty || !seg.empty())
            out.push_back(seg);
        if (pos == std::string_view::npos)
            return out;
        start = pos + delim.size();
    }
}

std::vector<std::string_view> SplitAny(std::string_view s, std::string_view delims, bool skip_empty)
{
    std::vector<std::string_view> out;
    std::size_t start = 0;
    for (;;)
    {
        const std::size_t pos = s.find_first_of(delims, start);
        const std::string_view seg = s.substr(start, pos == std::string_view::npos ? pos : pos - start);
        if (!skip_empty || !seg.empty())
            out.push_back(seg);
        if (pos == std::string_view::npos)
            return out;
        start = pos + 1;
    }
}

std::vector<std::string> SplitOwned(std::string_view s, char delim, bool skip_empty)
{
    const auto views = Split(s, delim, skip_empty);
    return {views.begin(), views.end()};
}

std::string_view TrimLeft(std::string_view s, std::string_view chars)
{
    const std::size_t pos = s.find_first_not_of(chars);
    return pos == std::string_view::npos ? std::string_view{} : s.substr(pos);
}

std::string_view TrimRight(std::string_view s, std::string_view chars)
{
    const std::size_t pos = s.find_last_not_of(chars);
    return pos == std::string_view::npos ? std::string_view{} : s.substr(0, pos + 1);
}

std::string_view Trim(std::string_view s, std::string_view chars) { return TrimRight(TrimLeft(s, chars), chars); }

std::string ToUpper(std::string_view s)
{
    std::string out(s);
    ToUpperInPlace(out);
    return out;
}

std::string ToLower(std::string_view s)
{
    std::string out(s);
    ToLowerInPlace(out);
    return out;
}

void ToUpperInPlace(std::string& s) noexcept
{
    for (char& c : s)
        c = UpperAscii(c);
}

void ToLowerInPlace(std::string& s) noexcept
{
    for (char& c : s)
        c = LowerAscii(c);
}

bool EqualsIgnoreCase(std::string_view a, std::string_view b) noexcept
{
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return LowerAscii(x) == LowerAscii(y); });
}

std::string Replace(std::string_view s, std::string_view from, std::string_view to, std::size_t max_count)
{
    if (from.empty())
        return std::string(s);
    std::string out;
    out.reserve(s.size());
    std::size_t start = 0;
    for (std::size_t done = 0; done < max_count; ++done)
    {
        const std::size_t pos = s.find(from, start);
        if (pos == std::string_view::npos)
            break;
        out.append(s.substr(start, pos - start));
        out.append(to);
        start = pos + from.size();
    }
    out.append(s.substr(start));
    return out;
}

std::string Repeat(std::string_view s, std::size_t n)
{
    std::string out;
    if (s.empty())
        return out;
    out.reserve(s.size() * n);
    for (std::size_t i = 0; i < n; ++i)
        out.append(s);
    return out;
}

std::optional<bool> ParseBool(std::string_view s) noexcept
{
    if (EqualsIgnoreCase(s, "true") || EqualsIgnoreCase(s, "yes") || EqualsIgnoreCase(s, "on") || s == "1")
        return true;
    if (EqualsIgnoreCase(s, "false") || EqualsIgnoreCase(s, "no") || EqualsIgnoreCase(s, "off") || s == "0")
        return false;
    return std::nullopt;
}

std::string FormatWithCommas(std::int64_t value)
{
    // 用无符号取绝对值：-INT64_MIN 在 int64 里溢出
    std::uint64_t mag = value < 0 ? 0 - static_cast<std::uint64_t>(value) : static_cast<std::uint64_t>(value);
    std::string digits = std::to_string(mag);
    std::string out;
    out.reserve(digits.size() + digits.size() / 3 + 1);
    if (value < 0)
        out += '-';
    const std::size_t lead = digits.size() % 3;
    for (std::size_t i = 0; i < digits.size(); ++i)
    {
        if (i != 0 && (i % 3) == lead)
            out += ',';
        out += digits[i];
    }
    return out;
}

} // namespace breeze::str
