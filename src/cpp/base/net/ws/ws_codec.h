// WebSocket（RFC 6455）的分帧编解码：交给 TcpServer / TcpClient 的 codec 工厂即得到 ws://；再叠 TLS 层即是 wss://。
// 设计见 docs/网络库设计.md（WebSocket 一节）。
//
//   TcpServer: opts.codec = std::make_shared<WsServerCodecFactory>(WsServerOptions{...})
//   TcpClient: opts.codec = std::make_shared<WsClientCodecFactory>(WsClientOptions{...})
//
// 握手（HTTP 升级）是 codec 自己的"握手来源"：OnConnected 要等升级完成才回调，握手超时、失败统计、重连退避都由连接层负责
// （ConnectionOptions::handshake_timeout、NetStats::handshake_failed）。IConnectionHandler::OnMessage 拿到的是一条完整的
// WebSocket 消息（分片已重组，控制帧已被 codec 吃掉）。
//
// 服务端 → 客户端的帧头只依赖长度，payload 仍然零拷贝共享，广播不退化；客户端 → 服务端要对 payload 加掩码，所以客户端的
// 连接每条消息拷贝一份（CodecTraits::encodes_payload）。掩码键用线程局部的 xoshiro 生成，不防浏览器场景的代理缓存投毒
// （客户端是机器人 / 压测 / 服务间调用，不是浏览器）。
//
// OnClosed 的 sys_errno：对端发 Close 帧时是它给的关闭码（无状态码为 1005），业务可用 4xxx 表示被踢 / 需要重新登录；
// 本端因协议错误关闭时是我们发出的关闭码（1002 / 1003 / 1007 / 1009）。优雅关闭（CloseGracefully）发 Close(1001)。

#pragma once

#include <memory>

#include "base/net/codec/frame_codec_interface.h"
#include "base/net/ws/ws_options.h"

namespace breeze
{

namespace ws
{
struct CodecShared; // 一个端点上全部连接共享的、不可变的配置（ws_codec.cpp）
}

// Create 线程安全（只读共享配置）。on_upgrade 会被多个 IO 线程并发调用，见 WsUpgradeCallback。
class WsServerCodecFactory final : public IFrameCodecFactory
{
public:
    explicit WsServerCodecFactory(WsServerOptions opts = {});
    ~WsServerCodecFactory() override;
    std::unique_ptr<IFrameCodec> Create() const override;

private:
    std::shared_ptr<const ws::CodecShared> shared_;
};

// 选项里的 host / path / origin / 子协议会原样写进升级请求：含控制字符（可注入头）或 path 不以 '/' 开头会被拒绝，
// 工厂记一条错误日志，此后每条连接在握手期失败。
class WsClientCodecFactory final : public IFrameCodecFactory
{
public:
    explicit WsClientCodecFactory(WsClientOptions opts = {});
    ~WsClientCodecFactory() override;
    std::unique_ptr<IFrameCodec> Create() const override;

private:
    std::shared_ptr<const ws::CodecShared> shared_;
};

} // namespace breeze
