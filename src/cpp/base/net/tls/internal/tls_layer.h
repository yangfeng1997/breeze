// 一条连接上的 TLS 层（BoringSSL 的 SSL*）。实现 IStreamLayer；由 TlsContext::Create 创建。属于 tls 的内部实现。
//
// 只在所属 loop 的线程上调用。BIO 只在两个缓冲之间搬字节（tls_bio.h）：线上字节从 Unwrap 的视图里直接读，
// 密文写进 out_（PendingWire）。renegotiation 保持 BoringSSL 的默认（从不）。
//
// 错误队列：BoringSSL 的错误队列是线程本地的，一个 IO 线程服务很多连接。BoringSSL 在每个 SSL_* 入口自己清它
// （ssl_reset_error_state），所以别的连接留下的陈旧错误不会让 SSL_get_error 把"数据没到齐"读成 SSL_ERROR_SSL——
// 这里因此**不再自己调 ERR_clear_error()**（对 BoringSSL 是冗余的；变异验证证实了这一点）。
// 若换成 OpenSSL（它不清），必须在每个入口前补上 ERR_clear_error()；tls_layer_test 的
// TestErrorQueueDoesNotLeakBetweenConnections / TestStaleThreadErrorDoesNotBreakUnwrap 守着这个性质，不管由谁来保证。

#pragma once

#include <cstddef>
#include <string>

#include "base/buffer/buffer.h"
#include "base/net/stream/stream_layer_interface.h"
#include "base/net/tls/internal/tls_bio.h"

struct ssl_st;

namespace breeze::tls
{

class TlsLayer final : public IStreamLayer
{
public:
    // 接管 ssl（析构时 SSL_free）。ssl 必须还没有设置 BIO，且已配好 SNI / 会话 / shed_handshake_config。
    TlsLayer(ssl_st* ssl, LayerRole role);
    ~TlsLayer() override;

    TlsLayer(const TlsLayer&) = delete;
    TlsLayer& operator=(const TlsLayer&) = delete;

    LayerStatus Start() override;
    LayerStatus Unwrap(std::span<std::byte> wire, PlainSink sink) override;
    LayerStatus Wrap(std::span<const std::byte> plain) override;
    std::span<const std::byte> PendingWire() const noexcept override { return out_.Readable(); }
    void ConsumeWire(std::size_t n) noexcept override;
    LayerStatus Shutdown() override;
    int error_code() const noexcept override { return error_; }

    // 以下给测试 / 诊断用。
    const ssl_st* native() const noexcept { return ssl_; }
    bool handshake_done() const noexcept { return handshake_done_; }
    bool session_reused() const noexcept;
    int protocol_version() const noexcept; // 0x0303 = TLS 1.2，0x0304 = TLS 1.3；握手完成前为 0
    // 对端证书的叶证书主题（"CN=..."），没有对端证书返回空串。
    std::string peer_subject() const;

private:
    LayerStatus Fail();

    ssl_st* ssl_;
    const bool client_;
    bool handshake_done_ = false;
    bool done_pending_ = false; // 握手刚完成、还没向连接报 HandshakeDone
    bool shutdown_sent_ = false;
    int error_ = 0;
    Buffer out_; // 待写出的线上字节（握手字节、密文）
    TlsBioState bio_state_;
};

} // namespace breeze::tls
