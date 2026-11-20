#include "base/net/tls/internal/tls_layer.h"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <string>

#include "base/logger/logger_macros.h"

namespace breeze::tls
{

namespace
{

// 一次 SSL_read 的明文暂存：一个 record 的明文上限，保证每次 SSL_read 不会留下 pending 的明文。
// 线程本地：一个 IO 线程上的全部连接共用；sink 里重入 Wrap（SSL_write）不碰它。
constexpr std::size_t kPlainChunk = kLayerMaxWrapBytes;

// 写缓冲带滞后地释放：握手字节（证书链等）可能有几 KB，之后连接空闲时不该一直占着。
constexpr std::size_t kShrinkAbove = 4 * 1024;

bool IsRetry(int ssl_error) noexcept { return ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE; }

} // namespace

TlsLayer::TlsLayer(ssl_st* ssl, LayerRole role)
    : ssl_(ssl)
    , client_(role == LayerRole::Client)
{
    bio_state_.out = &out_;
    BIO* bio = NewBio(&bio_state_);
    if (bio == nullptr)
        LOG_FATAL("TLS 的 BIO 创建失败（内存不足）");
    SSL_set_bio(ssl_, bio, bio); // 读写共用一个 BIO，SSL 接管这一份引用
    if (client_)
        SSL_set_connect_state(ssl_);
    else
        SSL_set_accept_state(ssl_);
}

TlsLayer::~TlsLayer()
{
    SSL_free(ssl_); // 要先于 bio_state_ / out_ 销毁：SSL 释放时会释放它持有的 BIO，BIO 的 destroy 虽然不碰 state，但别依赖这一点
}

LayerStatus TlsLayer::Fail()
{
    error_ = static_cast<int>(ERR_peek_last_error());
    if (error_ == 0)
        error_ = -1; // 错误队列是空的（例：Wrap 在握手完成之前被调用，SSL_write 返回了 WANT_READ）：也要给一个非 0 的码
    char text[160] = {};
    ERR_error_string_n(static_cast<std::uint32_t>(error_), text, sizeof(text));
    LOG_DEBUG("tls error: {} (verify_result={})", text, SSL_get_verify_result(ssl_));
    return LayerStatus::Error;
}

LayerStatus TlsLayer::Start()
{
    if (!client_)
        return LayerStatus::Ok; // 服务端等 ClientHello
    const int r = SSL_do_handshake(ssl_); // 产出 ClientHello 进 out_，随后 WANT_READ
    if (r == 1)
    {
        handshake_done_ = true; // 不会发生，但别漏
        return LayerStatus::HandshakeDone;
    }
    return IsRetry(SSL_get_error(ssl_, r)) ? LayerStatus::Ok : Fail();
}

LayerStatus TlsLayer::Unwrap(std::span<std::byte> wire, PlainSink sink)
{
    static thread_local std::byte plain[kPlainChunk];

    bio_state_.in = wire;
    LayerStatus result = LayerStatus::Ok;

    if (!handshake_done_)
    {
        const int r = SSL_do_handshake(ssl_);
        if (r == 1)
        {
            handshake_done_ = true;
            done_pending_ = true;
            if (!client_)
            {
                // TLS 1.3 服务端的 NewSessionTicket 不会随握手发出：BoringSSL 在 TCP 这类传输上把它推迟到服务端的第一次写，
                // "不向客户端写数据的使用方需要做一次零字节写才能把 ticket 刷出去"（tls13_server.cc 的注释）。
                // 不做的话，客户端先说、服务端不主动写的连接永远发不出 ticket，重连风暴时就恢复不了会话。
                // 实测：tls_layer_test 的 TestSessionResumption 在没有这一步时每次重连都是完整握手。
                if (SSL_write(ssl_, "", 0) < 0 && !IsRetry(SSL_get_error(ssl_, -1)))
                    result = Fail();
            }
        }
        else if (!IsRetry(SSL_get_error(ssl_, r)))
        {
            result = Fail();
        }
    }

    // 握手完成的同一批字节里可能就带着应用数据（TLS 1.3 客户端发完 Finished 立刻发数据）：接着读。
    if (handshake_done_ && result == LayerStatus::Ok)
    {
        for (;;)
        {
            const int n = SSL_read(ssl_, plain, static_cast<int>(sizeof(plain)));
            if (n > 0)
            {
                if (!sink({plain, static_cast<std::size_t>(n)}))
                    break; // 连接已经不可用，不再交付
                continue;
            }
            const int e = SSL_get_error(ssl_, n);
            if (e == SSL_ERROR_ZERO_RETURN)
                result = LayerStatus::PeerShutdown; // close_notify
            else if (!IsRetry(e))
                result = Fail();
            break;
        }
    }

    bio_state_.in = {}; // 视图只在这次调用里有效
    if (result == LayerStatus::Ok && done_pending_)
    {
        done_pending_ = false;
        return LayerStatus::HandshakeDone; // 可以晚于上面交付的明文：连接把"交付明文"也当作握手完成的信号
    }
    return result;
}

LayerStatus TlsLayer::Wrap(std::span<const std::byte> plain)
{
    if (plain.empty())
        return LayerStatus::Ok;
    // 不开 SSL_MODE_ENABLE_PARTIAL_WRITE：写全或出错。BIO 永远收得下，所以不会 WANT_WRITE。
    const int n = SSL_write(ssl_, plain.data(), static_cast<int>(plain.size()));
    if (n == static_cast<int>(plain.size()))
        return LayerStatus::Ok;
    return Fail();
}

void TlsLayer::ConsumeWire(std::size_t n) noexcept
{
    out_.Consume(n);
    if (out_.ReadableBytes() == 0 && out_.Capacity() > kShrinkAbove)
        out_.ShrinkIfEmpty(0);
}

LayerStatus TlsLayer::Shutdown()
{
    if (shutdown_sent_)
        return LayerStatus::Ok; // 幂等
    shutdown_sent_ = true;
    const int r = SSL_shutdown(ssl_); // 第一次调用发出 close_notify（返回 0）
    if (r >= 0)
        return LayerStatus::Ok;
    return IsRetry(SSL_get_error(ssl_, r)) ? LayerStatus::Ok : Fail();
}

bool TlsLayer::session_reused() const noexcept { return SSL_session_reused(ssl_) != 0; }

int TlsLayer::protocol_version() const noexcept { return handshake_done_ ? SSL_version(ssl_) : 0; }

std::string TlsLayer::peer_subject() const
{
    X509* cert = SSL_get_peer_certificate(ssl_);
    if (cert == nullptr)
        return {};
    char buf[256] = {};
    X509_NAME_oneline(X509_get_subject_name(cert), buf, sizeof(buf));
    X509_free(cert);
    return buf;
}

} // namespace breeze::tls
