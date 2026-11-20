// TLS 上下文：一个端点一份、不可变、被该端点的全部连接共享；实现 IStreamLayerFactory，交给
// StreamServerOptions::tls / StreamClientOptions::tls（及 TcpServerOptions / TcpClientOptions 里同名字段）即得到 tls://，
// 再配 WebSocket 的 codec 工厂即是 wss://。
//
//   std::string err;
//   auto ctx = TlsContext::CreateServer({.cert_chain = {.file = "server.pem"}, .private_key = {.file = "server.key"}}, err);
//   if (!ctx) { /* err 是原因 */ }
//   opts.tls = ctx;
//
// 本头不含任何 BoringSSL 类型，使用方不需要 openssl 的头文件。握手失败（证书不对、版本不合、握手超时）走连接层的握手阶段：
// 服务端只记 NetStats::handshake_failed[Tls]，没有 OnConnected / OnClosed；客户端按连接失败退避重试。
// **TLS 1.3 + mTLS 的时序**：客户端发完自己的证书与 Finished 就算握手完成，早于服务端校验客户端证书；服务端拒绝时客户端
// 一侧会先 OnConnected、随后收到 alert 以 TlsError 关闭，服务端一侧没有 OnConnected、只记 handshake_failed[Tls]。
// **握手失败在服务端的归类**：服务端自己判定的失败（明文打到 TLS 端口、客户端证书不对、版本 / 密码套件不合）记
// handshake_failed[Tls]；对端（客户端）因证书校验失败等原因自己中止握手并断开，服务端读到的是 EOF，记 handshake_failed[Io]。
// 看握手失败要把两项一起看。
// 握手完成之后的 record 错误以 CloseReason::TlsError 关闭。对端不发 close_notify 直接断开按 PeerClosed 处理
// （上层有自己的帧协议，不做截断攻击防护）。

#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "base/net/stream/stream_layer_interface.h"
#include "base/net/tls/tls_options.h"

struct ssl_ctx_st;

namespace breeze
{

struct TlsSessionBox; // 客户端最近一次会话的存放处（tls_context.cpp）；SSL_CTX 自己也持有一份引用，回调不依赖 TlsContext 的生命周期

class TlsContext final : public IStreamLayerFactory
{
public:
    // 失败返回空，error 是原因（证书 / 私钥读不出来、对不上、CA 无效 ……）。
    static std::shared_ptr<const TlsContext> CreateServer(const TlsServerOptions& options, std::string& error);
    static std::shared_ptr<const TlsContext> CreateClient(const TlsClientOptions& options, std::string& error);

    ~TlsContext() override;
    TlsContext(const TlsContext&) = delete;
    TlsContext& operator=(const TlsContext&) = delete;

    // 线程安全（多个 loop 的线程并发调用）。role 必须与创建方式一致（服务端上下文建 Server 层）：不一致是编程错误。
    std::unique_ptr<IStreamLayer> Create(LayerRole role) const override;

private:
    TlsContext(ssl_ctx_st* ctx, bool server);

    ssl_ctx_st* ctx_;
    const bool server_;

    // 客户端：校验与 SNI 的参数、最近一次会话（用来恢复下一次连接）。
    std::string server_name_;
    bool verify_peer_ = true;
    std::shared_ptr<TlsSessionBox> sessions_; // 空 = 不做会话恢复
};

// 热更证书：持有当前上下文，Reload 原子替换；之后新建的连接用新上下文，已建立的连接不受影响（它们各自持有 SSL_CTX 的引用）。
// 自己就是 IStreamLayerFactory，直接当 `tls` 选项用。不监听证书文件；NetModule 经 ReloadTls() 用它（framework/net/net_module_tls.cpp）。
class TlsContextHolder final : public IStreamLayerFactory
{
public:
    explicit TlsContextHolder(std::shared_ptr<const TlsContext> initial);

    void Reload(std::shared_ptr<const TlsContext> next);
    std::shared_ptr<const TlsContext> current() const { return current_.load(); }

    std::unique_ptr<IStreamLayer> Create(LayerRole role) const override;

private:
    std::atomic<std::shared_ptr<const TlsContext>> current_;
};

} // namespace breeze
