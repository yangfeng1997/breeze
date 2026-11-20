// HTTP/1.1 升级握手（RFC 6455 §4）的解析与构造。**严格子集，不是通用 HTTP 解析器**：握手来自不可信的对端，
// 宁可拒绝奇怪的写法也不去"宽容"它（宽容的解析器是请求走私的来源）。属于 ws 的内部实现，不是公开头。
//
// 严格在哪：只认 CRLF（裸 CR / 裸 LF 拒绝）；不接受 obs-fold（以空白开头的续行）；头名必须是 token、冒号前不许有空白；
// 值里不许有控制字符（HTAB 除外）；头总数有上限；Content-Length / Transfer-Encoding 一律拒绝（升级请求没有 body）；
// 会让语义含糊的头（Host / Origin / Sec-WebSocket-Key / Sec-WebSocket-Version 出现多次）拒绝。

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "base/net/ws/ws_options.h"

namespace breeze::ws
{

inline constexpr std::size_t kMaxHeaders = 64;
inline constexpr std::size_t kMaxSubprotocols = 16;
inline constexpr std::string_view kHandshakeGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// ---- 头部解析 ----

struct HttpHead
{
    std::string_view start_line; // 请求行或状态行，不含 CRLF
    std::array<WsHeader, kMaxHeaders> headers;
    std::size_t count = 0;

    // 同名头的第一行（名字大小写不敏感）；没有返回 nullptr。n 非空时写入同名的行数。
    const WsHeader* Find(std::string_view name, std::size_t* n = nullptr) const noexcept;
    // 所有同名行合起来，逗号分隔的令牌表里是否含 token（大小写不敏感）。
    bool HasToken(std::string_view name, std::string_view token) const noexcept;
};

enum class HeadParse : std::uint8_t
{
    Ok,
    Malformed,
    TooManyHeaders,
};

// 在 data 的前 limit 字节里找头部的结束（空行 \r\n\r\n），返回头部总长（含空行）；找不到返回 0。
// from：调用方已经确认前 from 字节里没有结束符，从 from 往前 3 字节处接着找（逐包到达时避免每次从头扫）。
std::size_t FindHeadEnd(std::span<const std::byte> data, std::size_t limit, std::size_t from = 0) noexcept;

// head 必须恰好以空行结尾（FindHeadEnd 的结果）。视图指向 head。
HeadParse ParseHead(std::string_view head, HttpHead& out) noexcept;

bool EqualsNoCase(std::string_view a, std::string_view b) noexcept;
// 逗号分隔的令牌表是否含 token（大小写不敏感，令牌两侧的空白忽略）。
bool ListHasToken(std::string_view list, std::string_view token) noexcept;

// ---- 服务端：判定升级请求 ----

enum class RequestVerdict : std::uint8_t
{
    Upgrade, // 合法的 WebSocket 升级请求
    Health,  // 健康检查路径上的普通 GET
    Reject,  // 回 status 并关闭
};

struct ServerRequest
{
    RequestVerdict verdict = RequestVerdict::Reject;
    int status = 400; // Reject 时的 HTTP 状态码
    std::string_view target;
    std::string_view path;
    std::string_view query;
    std::string_view host;
    std::string_view origin;
    std::string_view key; // Upgrade 时：Sec-WebSocket-Key（已验证是 16 字节的 base64）
    std::array<std::string_view, kMaxSubprotocols> subprotocols;
    std::size_t subprotocol_count = 0;
};

// 对 ParseHead 成功的请求头做全部判定：方法 / 版本 / 目标、Content-Length / Transfer-Encoding、Host、路由（health / path）、
// Upgrade / Connection、Sec-WebSocket-Version、Sec-WebSocket-Key、Origin。判定顺序固定，见 http_upgrade_test。
ServerRequest ClassifyUpgradeRequest(const HttpHead& head, const WsServerOptions& opts) noexcept;

// 在客户端提供的子协议里按服务端偏好挑一个；没有命中返回空串。
std::string_view ChooseSubprotocol(const std::vector<std::string>& server_preference,
                                   std::span<const std::string_view> offered) noexcept;

// base64(SHA-1(key + GUID))。
std::string ComputeAccept(std::string_view key);

std::string_view StatusText(int status) noexcept;
std::string BuildUpgradeResponse(std::string_view accept, std::string_view subprotocol);
// 426 自带 Upgrade / Sec-WebSocket-Version 头，405 带 Allow。一律 Content-Length: 0、Connection: close。
std::string BuildErrorResponse(int status);
std::string BuildHealthResponse();

// ---- 客户端 ----

// 请求里的各个值会原样写进头：调用方必须先用 IsSafeHeaderValue / IsSafeTarget 检查过。
std::string BuildUpgradeRequest(std::string_view host,
                                std::string_view path,
                                std::string_view origin,
                                const std::vector<std::string>& subprotocols,
                                std::string_view key);

// 值里没有控制字符（否则可以注入头）。
bool IsSafeHeaderValue(std::string_view v) noexcept;
// 请求目标：以 '/' 开头，没有空白与控制字符。
bool IsSafeTarget(std::string_view v) noexcept;
// RFC 7230 token（子协议名）。
bool IsToken(std::string_view v) noexcept;

struct ResponseCheck
{
    bool ok = false;
    int status = 0; // 状态行里的状态码；解析不出来为 0
};

// 校验 101 应答：状态行、Upgrade、Connection、Sec-WebSocket-Accept 与 expected_accept 逐字节相同、
// 没有 Sec-WebSocket-Extensions（我们没提供）、Sec-WebSocket-Protocol 若有必须是我们提供的之一。
ResponseCheck ValidateUpgradeResponse(const HttpHead& head,
                                      std::string_view expected_accept,
                                      const std::vector<std::string>& offered_subprotocols) noexcept;

} // namespace breeze::ws
