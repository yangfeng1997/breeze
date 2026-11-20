// WebSocket 的选项与升级回调的类型。设计见 docs/网络库设计.md（WebSocket 一节）。
//
// 库只做协议：HTTP 升级、帧、Ping / Pong / Close、分片、UTF-8。**不协商任何 extension**（含 permessage-deflate：
// 每连接一份 zlib 上下文，对游戏里的小消息得不偿失）；浏览器提供的 Sec-WebSocket-Extensions 被忽略，这是合法的。

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "base/net/connection/connection_context.h"
#include "base/net/endpoint/peer_info.h"

namespace breeze
{

// 数据消息用哪种帧。按端点配一种：出站用这种 opcode，入站收到另一种按 1003（不接受的数据类型）关闭。
// 浏览器的 ws.send(字符串) 发 Text、ws.send(ArrayBuffer) 发 Binary，游戏协议统一用 Binary。
enum class WsMessageType : std::uint8_t
{
    Binary,
    Text,
};

struct WsHeader
{
    std::string_view name;  // 原样大小写
    std::string_view value; // 已去掉首尾空白
};

// 升级请求。所有视图指向连接的读缓冲，只在 on_upgrade 回调期间有效；要保留就自己拷贝。
struct WsUpgradeRequest
{
    std::string_view target; // 请求目标原文，例："/game?token=abc"
    std::string_view path;   // target 里 '?' 之前
    std::string_view query;  // '?' 之后，不含 '?'；没有则为空
    std::string_view host;
    std::string_view origin;                         // 没有则为空（非浏览器客户端通常不带）
    std::span<const std::string_view> subprotocols;  // 客户端提供的 Sec-WebSocket-Protocol，已拆开、去空白
    std::span<const WsHeader> headers;               // 全部头
    const PeerInfo* peer = nullptr;

    // 大小写不敏感地取头；同名多行只返回第一行的值；没有返回空串。
    std::string_view Header(std::string_view name) const noexcept;
};

struct WsUpgradeReply
{
    // 0 = 接受升级；否则用这个 HTTP 状态码（必须在 400..599）拒绝并关闭连接，例：401 / 403 / 429 / 503。
    int reject_status = 0;
    // 接受时装到连接上（IConnection::GetContext 取），例：把 path / token / 真实 IP（X-Forwarded-For）解析成会话信息。
    std::unique_ptr<ConnectionContext> context;
};

// 升级回调：在 IO 线程上运行，**不得阻塞**、不得抛异常。可能被多个 IO 线程并发调用，所以是 const 可调用的
// （用 copyable_function 而不是 move_only_function：同一份回调被每个连接的 codec 共享，不能是只可移动的），
// 里面碰共享状态必须自己加锁或用原子。需要异步鉴权（查库、查 Redis）的放到首条业务消息里做，不要塞在这里。
using WsUpgradeCallback = std::copyable_function<void(const WsUpgradeRequest&, WsUpgradeReply&) const noexcept>;

struct WsServerOptions
{
    // 只接受这个路径（精确匹配，不含 query）；空 = 任意路径。路径不符回 404。
    std::string path;
    // 非空时，对该路径的普通 GET（不带升级头）回 200，给 L7 负载均衡做健康检查；回应之后关闭连接。
    std::string health_path;
    // 允许的 Origin（精确匹配，不分大小写）；空 = 不检查。请求没带 Origin 视为非浏览器客户端，放行。
    // 不匹配回 403。这是防浏览器页面的跨站 WebSocket 劫持，不是认证。
    std::vector<std::string> allowed_origins;
    // 服务端支持的子协议，按偏好排序；在客户端提供的里取第一个命中的写进应答。没有命中就不写
    // Sec-WebSocket-Protocol（规范允许，由客户端决定要不要继续）。
    std::vector<std::string> subprotocols;
    // 升级请求（请求行 + 全部头）的上限，超过回 431。
    std::size_t max_header_bytes = 8 * 1024;
    // 单条消息（含分片重组后）的上限。同时是读缓冲的上界：10 万连接都按它算内存，内部链路按需调大。
    // 帧头一解出来就检查，先于等待 payload。超过按 1009 关闭。
    std::size_t max_message_bytes = 64 * 1024;
    WsMessageType message_type = WsMessageType::Binary;
    // 仅 Text：校验 UTF-8（规范要求，非法按 1007 关闭）。交给上层自己校验时可关。
    bool validate_utf8 = true;
    WsUpgradeCallback on_upgrade;
};

struct WsClientOptions
{
    // Host 头。空 = 取对端地址（"ip:port"）。经反向代理 / 按域名路由的服务要填域名。
    std::string host;
    std::string path = "/";
    std::vector<std::string> subprotocols; // 提供给服务端的子协议；应答里的必须是其中之一
    std::string origin;                    // 空 = 不带 Origin 头
    std::size_t max_header_bytes = 8 * 1024;
    std::size_t max_message_bytes = 64 * 1024;
    WsMessageType message_type = WsMessageType::Binary;
    bool validate_utf8 = true;
};

} // namespace breeze
