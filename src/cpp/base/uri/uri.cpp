#include "base/uri/uri.h"

#include <cstddef>

namespace breeze
{

namespace
{

constexpr std::string_view kSubDelims = "!$&'()*+,;=";
constexpr std::size_t npos = std::string_view::npos;

constexpr bool IsAlpha(char c) noexcept { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
constexpr bool IsDigit(char c) noexcept { return c >= '0' && c <= '9'; }
constexpr bool IsHex(char c) noexcept { return IsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
constexpr bool IsUnreserved(char c) noexcept
{
    return IsAlpha(c) || IsDigit(c) || c == '-' || c == '.' || c == '_' || c == '~';
}
// 不能用 strchr：它会把结尾的 '\0' 也当成匹配。
bool IsSubDelim(char c) noexcept { return kSubDelims.find(c) != npos; }

constexpr char ToLowerAscii(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

// 逐字符检查：unreserved / sub-delims / 合法的 %XX / extra 里的字符；allow_high 时再放行 >= 0x80 的字节。
bool CheckChars(std::string_view s, std::string_view extra, bool allow_high) noexcept
{
    for (std::size_t i = 0; i < s.size(); ++i)
    {
        const char c = s[i];
        if (c == '%')
        {
            // 后面两位十六进制数字本身就是合法字符，不用跳过
            if (i + 2 >= s.size() || !IsHex(s[i + 1]) || !IsHex(s[i + 2]))
                return false;
        }
        else if (!(IsUnreserved(c) || IsSubDelim(c) || extra.find(c) != npos ||
                   (allow_high && static_cast<unsigned char>(c) >= 0x80)))
        {
            return false;
        }
    }
    return true;
}

bool IsScheme(std::string_view s) noexcept
{
    if (s.empty() || !IsAlpha(s[0]))
        return false;
    for (const char c : s)
    {
        if (!(IsAlpha(c) || IsDigit(c) || c == '+' || c == '-' || c == '.'))
            return false;
    }
    return true;
}

// 四段十进制，每段 1~3 位、不超过 255、除 "0" 外没有前导零。
bool IsIPv4(std::string_view s) noexcept
{
    int octets = 0;
    std::size_t pos = 0;
    while (true)
    {
        const std::size_t dot = s.find('.', pos);
        const std::string_view o = s.substr(pos, dot == npos ? npos : dot - pos);
        if (o.empty() || o.size() > 3)
            return false;
        int v = 0;
        for (const char c : o)
        {
            if (!IsDigit(c))
                return false;
            v = v * 10 + (c - '0');
        }
        if (v > 255 || (o.size() > 1 && o[0] == '0'))
            return false;
        ++octets;
        if (dot == npos)
            break;
        pos = dot + 1;
    }
    return octets == 4;
}

// "::" 一侧由冒号分隔的组：每组 1~4 位十六进制；整个地址的最后一组可以是点分 IPv4（算两个 16 位组）。
// 返回占用的 16 位组数，非法返回 -1；空串是 0 组。
int CountGroups(std::string_view side, bool may_end_with_ipv4) noexcept
{
    if (side.empty())
        return 0;
    int n = 0;
    std::size_t pos = 0;
    while (true)
    {
        const std::size_t colon = side.find(':', pos);
        const bool last = colon == npos;
        const std::string_view g = side.substr(pos, last ? npos : colon - pos);
        if (last && may_end_with_ipv4 && g.find('.') != npos)
        {
            if (!IsIPv4(g))
                return -1;
            n += 2;
        }
        else
        {
            if (g.empty() || g.size() > 4)
                return -1;
            for (const char c : g)
            {
                if (!IsHex(c))
                    return -1;
            }
            ++n;
        }
        if (last)
            return n;
        pos = colon + 1;
    }
}

// RFC 3986 的 IPv6address：没有 "::" 时恰好 8 组；有 "::" 时两侧合计不超过 7 组（"::" 至少顶一个零组）。
// 第二个 "::" 不用单独检查：它会落在右侧，那里的空组会被 CountGroups 拒绝（变异验证确认过这一点）。
bool IsIPv6(std::string_view s) noexcept
{
    const std::size_t dc = s.find("::");
    if (dc == npos)
        return CountGroups(s, true) == 8;
    const int left = CountGroups(s.substr(0, dc), false);
    const int right = CountGroups(s.substr(dc + 2), true);
    return left >= 0 && right >= 0 && left + right <= 7;
}

// IPvFuture = "v" 1*HEXDIG "." 1*( unreserved / sub-delims / ":" )
bool IsIPvFuture(std::string_view s) noexcept
{
    if (s.size() < 4 || (s[0] != 'v' && s[0] != 'V'))
        return false;
    std::size_t i = 1;
    while (i < s.size() && IsHex(s[i]))
        ++i;
    if (i == 1 || i >= s.size() || s[i] != '.')
        return false;
    ++i;
    if (i >= s.size())
        return false;
    for (; i < s.size(); ++i)
    {
        if (!(IsUnreserved(s[i]) || IsSubDelim(s[i]) || s[i] == ':'))
            return false;
    }
    return true;
}

// 解析 authority = [ userinfo "@" ] host [ ":" port ]，写进 u 的对应字段。
bool ParseAuthority(std::string_view auth,
                    std::optional<std::string>& userinfo,
                    std::optional<std::string>& host,
                    std::optional<std::uint16_t>& port)
{
    std::string_view hostport = auth;
    if (const std::size_t at = auth.find('@'); at != npos)
    {
        const std::string_view ui = auth.substr(0, at);
        if (!CheckChars(ui, ":", false))
            return false;
        userinfo = std::string(ui);
        hostport = auth.substr(at + 1); // 里面如果还有 '@'，下面的字符集检查会拒绝
    }

    std::string_view host_text;
    std::string_view port_text;
    if (!hostport.empty() && hostport.front() == '[')
    {
        const std::size_t close = hostport.find(']');
        if (close == npos)
            return false;
        host_text = hostport.substr(0, close + 1);
        const std::string_view inner = host_text.substr(1, host_text.size() - 2);
        if (!IsIPv6(inner) && !IsIPvFuture(inner))
            return false;
        const std::string_view after = hostport.substr(close + 1);
        if (!after.empty())
        {
            if (after.front() != ':')
                return false;
            port_text = after.substr(1);
        }
    }
    else
    {
        const std::size_t colon = hostport.find(':');
        host_text = hostport.substr(0, colon);
        if (colon != npos)
            port_text = hostport.substr(colon + 1);
        if (!CheckChars(host_text, "", false))
            return false;
    }
    host = std::string(host_text);

    // 空端口（"h:"）是合法的，当作没写。
    std::uint32_t value = 0;
    for (const char c : port_text)
    {
        if (!IsDigit(c))
            return false;
        value = value * 10 + static_cast<std::uint32_t>(c - '0');
        if (value > 65535)
            return false; // 每步都检查，长串数字不会溢出
    }
    if (!port_text.empty())
        port = static_cast<std::uint16_t>(value);
    return true;
}

constexpr char kHexUpper[] = "0123456789ABCDEF";

constexpr int HexValue(char c) noexcept
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return c - 'A' + 10;
}

} // namespace

std::optional<Uri> Uri::Parse(std::string_view text)
{
    Uri u;
    std::string_view rest = text;

    // 先按 '#'、'?' 从后往前切：fragment 和 query 里不会再出现这两个字符，剩下的部分不用再管它们。
    if (const std::size_t p = rest.find('#'); p != npos)
    {
        const std::string_view frag = rest.substr(p + 1);
        if (!CheckChars(frag, ":@/?", true))
            return std::nullopt;
        u.fragment_ = std::string(frag);
        rest = rest.substr(0, p);
    }
    if (const std::size_t p = rest.find('?'); p != npos)
    {
        const std::string_view query = rest.substr(p + 1);
        if (!CheckChars(query, ":@/?", true))
            return std::nullopt;
        u.query_ = std::string(query);
        rest = rest.substr(0, p);
    }

    // 第一个 ':' 出现在第一个 '/' 之前，前面的部分就必须是 scheme；
    // 不是合法 scheme 的话，它只可能是"第一段带冒号的相对路径"，RFC 明确禁止（"1a:b"、":x" 都不合法）。
    if (const std::size_t p = rest.find_first_of(":/"); p != npos && rest[p] == ':')
    {
        const std::string_view scheme = rest.substr(0, p);
        if (!IsScheme(scheme))
            return std::nullopt;
        u.scheme_.reserve(scheme.size());
        for (const char c : scheme)
            u.scheme_.push_back(ToLowerAscii(c));
        rest.remove_prefix(p + 1);
    }

    if (rest.starts_with("//"))
    {
        rest.remove_prefix(2);
        const std::size_t end = rest.find('/'); // '?' 和 '#' 已经切掉了
        const std::string_view auth = rest.substr(0, end);
        rest = end == npos ? std::string_view() : rest.substr(end);
        if (!ParseAuthority(auth, u.userinfo_, u.host_, u.port_))
            return std::nullopt;
    }

    // 剩下的是 path：有 authority 时必然是空或以 '/' 开头；没有时不会以 "//" 开头（那会被当成 authority）。
    if (!CheckChars(rest, ":@/", true))
        return std::nullopt;
    u.path_ = std::string(rest);
    return u;
}

std::string_view Uri::hostname() const noexcept
{
    if (!host_.has_value())
        return {};
    std::string_view h = *host_;
    if (!h.empty() && h.front() == '[')
        h = h.substr(1, h.size() - 2);
    return h;
}

std::uint16_t Uri::DefaultPort(std::string_view scheme) noexcept
{
    char buf[8];
    if (scheme.size() > sizeof(buf))
        return 0;
    for (std::size_t i = 0; i < scheme.size(); ++i)
        buf[i] = ToLowerAscii(scheme[i]);
    const std::string_view s(buf, scheme.size());
    if (s == "http" || s == "ws")
        return 80;
    if (s == "https" || s == "wss")
        return 443;
    return 0;
}

std::string Uri::ToString() const
{
    std::string out;
    if (!scheme_.empty())
    {
        out += scheme_;
        out += ':';
    }
    if (host_.has_value())
    {
        out += "//";
        if (userinfo_.has_value())
        {
            out += *userinfo_;
            out += '@';
        }
        out += *host_;
        if (port_.has_value())
        {
            out += ':';
            out += std::to_string(*port_);
        }
    }
    out += path_;
    if (query_.has_value())
    {
        out += '?';
        out += *query_;
    }
    if (fragment_.has_value())
    {
        out += '#';
        out += *fragment_;
    }
    return out;
}

std::string PercentEncode(std::string_view s, bool space_as_plus)
{
    std::string out;
    out.reserve(s.size());
    for (const char c : s)
    {
        if (IsUnreserved(c))
        {
            out.push_back(c);
        }
        else if (c == ' ' && space_as_plus)
        {
            out.push_back('+');
        }
        else
        {
            const auto b = static_cast<unsigned char>(c);
            out.push_back('%');
            out.push_back(kHexUpper[b >> 4]);
            out.push_back(kHexUpper[b & 15]);
        }
    }
    return out;
}

std::optional<std::string> PercentDecode(std::string_view s, bool plus_as_space)
{
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i)
    {
        const char c = s[i];
        if (c == '%')
        {
            if (i + 2 >= s.size() || !IsHex(s[i + 1]) || !IsHex(s[i + 2]))
                return std::nullopt;
            out.push_back(static_cast<char>(HexValue(s[i + 1]) * 16 + HexValue(s[i + 2])));
            i += 2;
        }
        else if (c == '+' && plus_as_space)
        {
            out.push_back(' ');
        }
        else
        {
            out.push_back(c);
        }
    }
    return out;
}

std::optional<std::vector<std::pair<std::string, std::string>>> ParseQuery(std::string_view query)
{
    std::vector<std::pair<std::string, std::string>> out;
    std::size_t pos = 0;
    while (pos <= query.size())
    {
        const std::size_t amp = query.find('&', pos);
        const std::string_view seg = query.substr(pos, amp == npos ? npos : amp - pos);
        if (!seg.empty())
        {
            const std::size_t eq = seg.find('=');
            auto key = PercentDecode(seg.substr(0, eq), true);
            auto value = PercentDecode(eq == npos ? std::string_view() : seg.substr(eq + 1), true);
            if (!key || !value)
                return std::nullopt;
            out.emplace_back(std::move(*key), std::move(*value));
        }
        if (amp == npos)
            break;
        pos = amp + 1;
    }
    return out;
}

} // namespace breeze
