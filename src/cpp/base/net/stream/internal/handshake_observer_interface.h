// 连接在握手期（OnConnected 之前）就死掉时的内部通知。只有 Connector 用：它靠 OnClosed 驱动重连，
// 而没回调过 OnConnected 的连接没有 OnClosed，握手失败要靠这个才能触发退避重连与 on_connect_failed。
// 服务端不传（握手失败只记统计与日志）。设计见方案 §3.2。

#pragma once

namespace breeze
{

class IHandshakeObserver
{
public:
    virtual ~IHandshakeObserver() = default;

    // 连接在 OnConnected 之前关闭：握手失败 / 超时 / 对端中途断开 / 被 Stop 或停机关闭。恰触发一次，在连接回收时
    // （不在 Close 的调用栈里），随后连接才销毁。
    // mapped_errno 是固定映射出的 errno（超时 ETIMEDOUT、TLS / 协议错误 EPROTO、对端断开 ECONNRESET ……），
    // 不是 BoringSSL 的错误码：值域与 errno 重叠的话，碰巧等于 EINVAL / EPERM 的会被 Connector 当成"不可重试"，
    // 客户端就永久停止重连了。详细原因只进日志。
    virtual void OnHandshakeFailed(int mapped_errno) noexcept = 0;
};

} // namespace breeze
