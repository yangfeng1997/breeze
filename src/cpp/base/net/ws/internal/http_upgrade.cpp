#include "base/net/ws/internal/http_upgrade.h"

#include <algorithm>

#include "base/hash/sha1.h"
#include "base/string/base64.h"

namespace breeze
{

// 公开类型 WsUpgradeRequest 的方法，实现放在这里（要用大小写不敏感比较）。
std::string_view WsUpgradeRequest::Header(std::string_view name) const noexcept
{
    for (const WsHeader& h : headers)
        if (ws::EqualsNoCase(h.name, name))
            return h.value;
    return {};
}

} // namespace breeze

namespace breeze::ws
{

namespace
{

constexpr bool IsTChar(char c) noexcept
{
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
        return true;
    switch (c)
    {
        case '!':
        case '#':
        case '$':
        case '%':
        case '&':
        case '\'':
        case '*':
        case '+':
        case '-':
        case '.':
        case '^':
        case '_':
        case '`':
        case '|':
        case '~':
            return true;
        default:
            return false;
    }
}

constexpr char Lower(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }
constexpr bool IsOws(char c) noexcept { return c == ' ' || c == '\t'; }
constexpr bool IsCtl(char c) noexcept { return static_cast<unsigned char>(c) < 0x20 || c == 0x7F; }

std::string_view TrimOws(std::string_view s) noexcept
{
    while (!s.empty() && IsOws(s.front()))
        s.remove_prefix(1);
    while (!s.empty() && IsOws(s.back()))
        s.remove_suffix(1);
    return s;
}

// 值里允许 HTAB，不允许别的控制字符（含 NUL）。
bool ValueHasBadChar(std::string_view v) noexcept
{
    return std::ranges::any_of(v, [](char c) { return IsCtl(c) && c != '\t'; });
}

} // namespace

bool EqualsNoCase(std::string_view a, std::string_view b) noexcept
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (Lower(a[i]) != Lower(b[i]))
            return false;
    return true;
}

bool ListHasToken(std::string_view list, std::string_view token) noexcept
{
    while (!list.empty())
    {
        const std::size_t comma = list.find(',');
        if (EqualsNoCase(TrimOws(list.substr(0, comma)), token))
            return true;
        if (comma == std::string_view::npos)
            break;
        list.remove_prefix(comma + 1);
    }
    return false;
}

bool IsSafeHeaderValue(std::string_view v) noexcept { return !std::ranges::any_of(v, IsCtl); }

bool IsSafeTarget(std::string_view v) noexcept
{
    return !v.empty() && v.front() == '/' &&
           !std::ranges::any_of(v, [](char c) { return static_cast<unsigned char>(c) <= 0x20 || c == 0x7F; });
}

bool IsToken(std::string_view v) noexcept { return !v.empty() && std::ranges::all_of(v, IsTChar); }

// ---- 头部解析 ----

const WsHeader* HttpHead::Find(std::string_view name, std::size_t* n) const noexcept
{
    const WsHeader* first = nullptr;
    std::size_t c = 0;
    for (std::size_t i = 0; i < count; ++i)
    {
        if (!EqualsNoCase(headers[i].name, name))
            continue;
        if (first == nullptr)
            first = &headers[i];
        ++c;
    }
    if (n != nullptr)
        *n = c;
    return first;
}

bool HttpHead::HasToken(std::string_view name, std::string_view token) const noexcept
{
    for (std::size_t i = 0; i < count; ++i)
        if (EqualsNoCase(headers[i].name, name) && ListHasToken(headers[i].value, token))
            return true;
    return false;
}

std::size_t FindHeadEnd(std::span<const std::byte> data, std::size_t limit, std::size_t from) noexcept
{
    const std::size_t n = std::min(data.size(), limit);
    for (std::size_t i = from >= 3 ? from - 3 : 0; i + 4 <= n; ++i)
    {
        if (data[i] == std::byte{'\r'} && data[i + 1] == std::byte{'\n'} && data[i + 2] == std::byte{'\r'} &&
            data[i + 3] == std::byte{'\n'})
            return i + 4;
    }
    return 0;
}

HeadParse ParseHead(std::string_view head, HttpHead& out) noexcept
{
    out.count = 0;
    out.start_line = {};
    if (head.size() < 4 || head.substr(head.size() - 4) != "\r\n\r\n")
        return HeadParse::Malformed;

    std::size_t pos = 0;
    bool first = true;
    for (;;)
    {
        const std::size_t eol = head.find("\r\n", pos);
        if (eol == std::string_view::npos)
            return HeadParse::Malformed;
        const std::string_view line = head.substr(pos, eol - pos);
        pos = eol + 2;

        if (line.empty())
            return (!first && pos == head.size()) ? HeadParse::Ok : HeadParse::Malformed; // 空行必须是最后的结束符

        // 裸 CR / 裸 LF（行内出现、后面没跟配对的那一半）不用单独检查：它们落在起始行（控制字符）、头名（不是 token 字符）
        // 或头值（控制字符）里，下面三处字符检查各自会拒掉。http_upgrade_test 的 TestParseRejects 逐一覆盖。
        if (first)
        {
            if (std::ranges::any_of(line, IsCtl))
                return HeadParse::Malformed;
            out.start_line = line;
            first = false;
            continue;
        }

        // obs-fold（以空白开头的续行）同理：它的"头名"以空白开头，过不了下面的 token 检查；"Host :" 也一样。
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0)
            return HeadParse::Malformed;
        const std::string_view name = line.substr(0, colon);
        if (!std::ranges::all_of(name, IsTChar))
            return HeadParse::Malformed;
        const std::string_view value = TrimOws(line.substr(colon + 1));
        if (ValueHasBadChar(value))
            return HeadParse::Malformed;
        if (out.count == kMaxHeaders)
            return HeadParse::TooManyHeaders;
        out.headers[out.count++] = {name, value};
    }
}

// ---- 服务端 ----

ServerRequest ClassifyUpgradeRequest(const HttpHead& head, const WsServerOptions& opts) noexcept
{
    ServerRequest r;
    const auto reject = [&r](int status)
    {
        r.verdict = RequestVerdict::Reject;
        r.status = status;
        return r;
    };

    // 请求行："METHOD SP target SP HTTP/1.1"，恰好两个空格
    const std::string_view line = head.start_line;
    const std::size_t sp1 = line.find(' ');
    const std::size_t sp2 = sp1 == std::string_view::npos ? sp1 : line.find(' ', sp1 + 1);
    if (sp2 == std::string_view::npos)
        return reject(400);
    const std::string_view method = line.substr(0, sp1);
    const std::string_view target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    const std::string_view version = line.substr(sp2 + 1);
    if (!IsToken(method) || version.find(' ') != std::string_view::npos)
        return reject(400);
    if (method != "GET")
        return reject(405);
    if (version != "HTTP/1.1" || !IsSafeTarget(target))
        return reject(400);

    // 升级请求没有 body：带 Content-Length / Transfer-Encoding 的一律拒绝（请求走私的入口）。
    if (head.Find("content-length") != nullptr || head.Find("transfer-encoding") != nullptr)
        return reject(400);

    std::size_t n = 0;
    const WsHeader* host = head.Find("host", &n);
    if (host == nullptr || n != 1 || host->value.empty())
        return reject(400);

    r.target = target;
    const std::size_t q = target.find('?');
    r.path = target.substr(0, q);
    r.query = q == std::string_view::npos ? std::string_view{} : target.substr(q + 1);
    r.host = host->value;

    // 路由：健康检查路径上不带 Upgrade 头的 GET；其余必须落在服务的路径上
    if (!opts.health_path.empty() && r.path == opts.health_path && head.Find("upgrade") == nullptr)
    {
        r.verdict = RequestVerdict::Health;
        r.status = 200;
        return r;
    }
    if (!opts.path.empty() && r.path != opts.path)
        return reject(404);

    if (!head.HasToken("upgrade", "websocket") || !head.HasToken("connection", "upgrade"))
        return reject(426);
    const WsHeader* version_hdr = head.Find("sec-websocket-version", &n);
    if (version_hdr == nullptr || n != 1 || version_hdr->value != "13")
        return reject(426); // 应答里带 Sec-WebSocket-Version: 13，见 BuildErrorResponse

    const WsHeader* key = head.Find("sec-websocket-key", &n);
    if (key == nullptr || n != 1 || key->value.size() != 24)
        return reject(400);
    const auto raw = str::Base64Decode(key->value);
    if (!raw || raw->size() != 16)
        return reject(400);
    r.key = key->value;

    const WsHeader* origin = head.Find("origin", &n);
    if (n > 1)
        return reject(400);
    if (origin != nullptr)
    {
        r.origin = origin->value;
        if (!opts.allowed_origins.empty() &&
            !std::ranges::any_of(opts.allowed_origins, [&](const std::string& o) { return EqualsNoCase(o, origin->value); }))
            return reject(403);
    }

    // 子协议：所有 Sec-WebSocket-Protocol 行合起来的令牌表
    for (std::size_t i = 0; i < head.count; ++i)
    {
        if (!EqualsNoCase(head.headers[i].name, "sec-websocket-protocol"))
            continue;
        std::string_view list = head.headers[i].value;
        while (!list.empty())
        {
            const std::size_t comma = list.find(',');
            const std::string_view item = TrimOws(list.substr(0, comma));
            if (!item.empty())
            {
                if (!IsToken(item) || r.subprotocol_count == kMaxSubprotocols)
                    return reject(400);
                r.subprotocols[r.subprotocol_count++] = item;
            }
            if (comma == std::string_view::npos)
                break;
            list.remove_prefix(comma + 1);
        }
    }

    r.verdict = RequestVerdict::Upgrade;
    r.status = 101;
    return r;
}

std::string_view ChooseSubprotocol(const std::vector<std::string>& server_preference,
                                   std::span<const std::string_view> offered) noexcept
{
    for (const std::string& pref : server_preference)
        for (const std::string_view o : offered)
            if (o == pref) // 子协议名区分大小写
                return pref;
    return {};
}

std::string ComputeAccept(std::string_view key)
{
    Sha1 sha;
    sha.Update(key);
    sha.Update(kHandshakeGuid);
    const Sha1::Digest d = sha.Final();
    return str::Base64Encode(std::as_bytes(std::span(d)));
}

std::string_view StatusText(int status) noexcept
{
    switch (status)
    {
        case 101:
            return "Switching Protocols";
        case 200:
            return "OK";
        case 400:
            return "Bad Request";
        case 401:
            return "Unauthorized";
        case 403:
            return "Forbidden";
        case 404:
            return "Not Found";
        case 405:
            return "Method Not Allowed";
        case 408:
            return "Request Timeout";
        case 409:
            return "Conflict";
        case 426:
            return "Upgrade Required";
        case 429:
            return "Too Many Requests";
        case 431:
            return "Request Header Fields Too Large";
        case 500:
            return "Internal Server Error";
        case 502:
            return "Bad Gateway";
        case 503:
            return "Service Unavailable";
        default:
            return "Error";
    }
}

std::string BuildUpgradeResponse(std::string_view accept, std::string_view subprotocol)
{
    std::string out;
    out.reserve(160);
    out += "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ";
    out += accept;
    out += "\r\n";
    if (!subprotocol.empty())
    {
        out += "Sec-WebSocket-Protocol: ";
        out += subprotocol;
        out += "\r\n";
    }
    out += "\r\n";
    return out;
}

std::string BuildErrorResponse(int status)
{
    std::string out = "HTTP/1.1 ";
    out += std::to_string(status);
    out += ' ';
    out += StatusText(status);
    out += "\r\n";
    if (status == 426)
        out += "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\n";
    else
        out += "Connection: close\r\n";
    if (status == 405)
        out += "Allow: GET\r\n";
    out += "Content-Length: 0\r\n\r\n";
    return out;
}

std::string BuildHealthResponse()
{
    return "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: text/plain\r\nContent-Length: 2\r\n\r\nok";
}

// ---- 客户端 ----

std::string BuildUpgradeRequest(std::string_view host,
                                std::string_view path,
                                std::string_view origin,
                                const std::vector<std::string>& subprotocols,
                                std::string_view key)
{
    std::string out;
    out.reserve(256);
    out += "GET ";
    out += path;
    out += " HTTP/1.1\r\nHost: ";
    out += host;
    out += "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ";
    out += key;
    out += "\r\nSec-WebSocket-Version: 13\r\n";
    if (!origin.empty())
    {
        out += "Origin: ";
        out += origin;
        out += "\r\n";
    }
    if (!subprotocols.empty())
    {
        out += "Sec-WebSocket-Protocol: ";
        for (std::size_t i = 0; i < subprotocols.size(); ++i)
        {
            if (i != 0)
                out += ", ";
            out += subprotocols[i];
        }
        out += "\r\n";
    }
    out += "\r\n";
    return out;
}

ResponseCheck ValidateUpgradeResponse(const HttpHead& head,
                                      std::string_view expected_accept,
                                      const std::vector<std::string>& offered_subprotocols) noexcept
{
    ResponseCheck r;

    // 状态行："HTTP/1.1 SP 3 位数字 [SP reason]"
    const std::string_view line = head.start_line;
    if (line.size() < 12 || line.substr(0, 9) != "HTTP/1.1 ")
        return r;
    const std::string_view code = line.substr(9, 3);
    if (!std::ranges::all_of(code, [](char c) { return c >= '0' && c <= '9'; }) || (line.size() > 12 && line[12] != ' '))
        return r;
    r.status = (code[0] - '0') * 100 + (code[1] - '0') * 10 + (code[2] - '0');
    if (r.status != 101)
        return r;

    if (!head.HasToken("upgrade", "websocket") || !head.HasToken("connection", "upgrade"))
        return r;

    std::size_t n = 0;
    const WsHeader* accept = head.Find("sec-websocket-accept", &n);
    if (accept == nullptr || n != 1 || accept->value != expected_accept)
        return r;
    if (head.Find("sec-websocket-extensions") != nullptr)
        return r; // 我们没有提供任何 extension，服务端不能自作主张启用

    const WsHeader* proto = head.Find("sec-websocket-protocol", &n);
    if (proto != nullptr)
    {
        if (n != 1 || !std::ranges::any_of(offered_subprotocols, [&](const std::string& s) { return s == proto->value; }))
            return r; // 应答的子协议必须是我们提供的之一
    }
    r.ok = true;
    return r;
}

} // namespace breeze::ws
