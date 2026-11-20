#include "base/net/tls/tls_context.h"

#include <arpa/inet.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <cstdint>
#include <fstream>
#include <mutex>
#include <sstream>
#include <vector>

#include "base/logger/logger_macros.h"
#include "base/net/tls/internal/tls_layer.h"

namespace breeze
{

// 客户端最近一次收到的会话（session ticket）。TlsContext 与 SSL_CTX（经 ex_data）各持有一份引用：
// 回调只认这个对象，不认 TlsContext，所以 TlsContext 先于连接销毁（热更证书）也不会悬空。
struct TlsSessionBox
{
    std::mutex mu;
    SSL_SESSION* last = nullptr;

    ~TlsSessionBox()
    {
        if (last != nullptr)
            SSL_SESSION_free(last);
    }
};

namespace
{

std::string OpenSslError()
{
    const std::uint32_t e = ERR_peek_last_error();
    if (e == 0)
        return "未知错误";
    char buf[160] = {};
    ERR_error_string_n(e, buf, sizeof(buf));
    return buf;
}

enum class Pem : std::uint8_t
{
    Absent, // 没配置
    Ok,
    Failed, // 配了但读不出来 / 不合法，error 已写
};

// 取一份 PEM 的文本：pem 优先，否则读文件。
Pem LoadPem(const TlsPemSource& src, const char* what, std::string& text, std::string& error)
{
    if (!src.pem.empty())
    {
        text = src.pem;
        return Pem::Ok;
    }
    if (src.file.empty())
        return Pem::Absent;
    std::ifstream f(src.file, std::ios::binary);
    if (!f)
    {
        error = std::string(what) + "：打不开文件 " + src.file;
        return Pem::Failed;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    text = ss.str();
    if (text.empty())
    {
        error = std::string(what) + "：文件是空的 " + src.file;
        return Pem::Failed;
    }
    return Pem::Ok;
}

std::vector<bssl::UniquePtr<X509>> ParseCerts(const std::string& pem)
{
    std::vector<bssl::UniquePtr<X509>> out;
    bssl::UniquePtr<BIO> bio(BIO_new_mem_buf(pem.data(), static_cast<ossl_ssize_t>(pem.size())));
    if (!bio)
        return out;
    while (X509* x = PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr))
        out.emplace_back(x);
    ERR_clear_error(); // 读到末尾的 "no start line" 不是错误
    return out;
}

// 把证书链与私钥装进 ctx。两者都没配置返回 Absent；只配一个、读不出来、对不上返回 Failed。
Pem LoadIdentity(SSL_CTX* ctx, const TlsPemSource& cert, const TlsPemSource& key, std::string& error)
{
    std::string cert_pem;
    std::string key_pem;
    const Pem c = LoadPem(cert, "cert_chain", cert_pem, error);
    if (c == Pem::Failed)
        return Pem::Failed;
    const Pem k = LoadPem(key, "private_key", key_pem, error);
    if (k == Pem::Failed)
        return Pem::Failed;
    if (c == Pem::Absent && k == Pem::Absent)
        return Pem::Absent;
    if (c == Pem::Absent || k == Pem::Absent)
    {
        error = "cert_chain 与 private_key 必须一起配置";
        return Pem::Failed;
    }

    const auto chain = ParseCerts(cert_pem);
    if (chain.empty())
    {
        error = "cert_chain 里没有可解析的 PEM 证书";
        return Pem::Failed;
    }
    bssl::UniquePtr<BIO> kbio(BIO_new_mem_buf(key_pem.data(), static_cast<ossl_ssize_t>(key_pem.size())));
    bssl::UniquePtr<EVP_PKEY> pkey(kbio ? PEM_read_bio_PrivateKey(kbio.get(), nullptr, nullptr, nullptr) : nullptr);
    if (!pkey)
    {
        error = "private_key 解析失败（不是 PEM？带口令的私钥不支持）：" + OpenSslError();
        return Pem::Failed;
    }
    if (SSL_CTX_use_certificate(ctx, chain[0].get()) != 1)
    {
        error = "装入叶证书失败：" + OpenSslError();
        return Pem::Failed;
    }
    for (std::size_t i = 1; i < chain.size(); ++i)
    {
        if (SSL_CTX_add1_chain_cert(ctx, chain[i].get()) != 1)
        {
            error = "装入中间证书失败：" + OpenSslError();
            return Pem::Failed;
        }
    }
    // SSL_CTX_use_PrivateKey 自己会校验私钥与叶证书配对（不配对返回错误），所以不再单独调 SSL_CTX_check_private_key。
    if (SSL_CTX_use_PrivateKey(ctx, pkey.get()) != 1)
    {
        error = "装入私钥失败（私钥与证书不配对？）：" + OpenSslError();
        return Pem::Failed;
    }
    return Pem::Ok;
}

bssl::UniquePtr<SSL_CTX> NewCtx(TlsVersion min_version, TlsVersion max_version, std::string& error)
{
    bssl::UniquePtr<SSL_CTX> ctx(SSL_CTX_new(TLS_method()));
    if (!ctx)
    {
        error = "SSL_CTX_new 失败：" + OpenSslError();
        return nullptr;
    }
    if (SSL_CTX_set_min_proto_version(ctx.get(), min_version == TlsVersion::Tls13 ? TLS1_3_VERSION : TLS1_2_VERSION) != 1 ||
        SSL_CTX_set_max_proto_version(ctx.get(), max_version == TlsVersion::Tls12 ? TLS1_2_VERSION : TLS1_3_VERSION) != 1)
    {
        error = "设置协议版本失败：" + OpenSslError();
        return nullptr;
    }
    // groups 只开 X25519 + P-256，不跟 BoringSSL 的默认去选后量子混合的 X25519MLKEM768：服务端多约 40% CPU、握手包更大。
    // 客户端也只给这两个，于是首个 key share 就能被接受，不会多一次 HelloRetryRequest 往返。
    static constexpr std::uint16_t kGroups[] = {SSL_GROUP_X25519, SSL_GROUP_SECP256R1};
    if (SSL_CTX_set1_group_ids(ctx.get(), kGroups, sizeof(kGroups) / sizeof(kGroups[0])) != 1)
    {
        error = "设置 groups 失败：" + OpenSslError();
        return nullptr;
    }
    return ctx;
}

// 一份 CA 的 PEM 装进信任库；返回装了几张，0 = 没有可解析的证书。names 非空时把它们的主题也收进去（给 CertificateRequest）。
std::size_t AddCas(SSL_CTX* ctx, const std::string& pem, STACK_OF(X509_NAME) * names)
{
    const auto cas = ParseCerts(pem);
    X509_STORE* store = SSL_CTX_get_cert_store(ctx);
    for (const auto& c : cas)
    {
        X509_STORE_add_cert(store, c.get());
        if (names != nullptr)
            sk_X509_NAME_push(names, X509_NAME_dup(X509_get_subject_name(c.get())));
    }
    ERR_clear_error(); // 重复添加同一张 CA 会留下 "cert already in hash table"，不是错误
    return cas.size();
}

bool IsIpLiteral(const std::string& s)
{
    in6_addr a6;
    in_addr a4;
    return ::inet_pton(AF_INET, s.c_str(), &a4) == 1 || ::inet_pton(AF_INET6, s.c_str(), &a6) == 1;
}

// SSL_CTX 的 ex_data：持有 TlsSessionBox 的一份引用。
void FreeBox(void*, void* ptr, CRYPTO_EX_DATA*, int, long, void*) { delete static_cast<std::shared_ptr<TlsSessionBox>*>(ptr); }

int BoxIndex()
{
    static const int index = SSL_CTX_get_ex_new_index(0, nullptr, nullptr, nullptr, FreeBox);
    return index;
}

// SSL_CTX_sess_set_new_cb：客户端收到服务端的 session ticket。返回 1 = 我们接管这份引用。
int OnNewSession(SSL* ssl, SSL_SESSION* session)
{
    auto* box = static_cast<std::shared_ptr<TlsSessionBox>*>(SSL_CTX_get_ex_data(SSL_get_SSL_CTX(ssl), BoxIndex()));
    if (box == nullptr || !*box)
        return 0;
    const std::lock_guard lk((*box)->mu);
    if ((*box)->last != nullptr)
        SSL_SESSION_free((*box)->last);
    (*box)->last = session;
    return 1;
}

} // namespace

TlsContext::TlsContext(ssl_ctx_st* ctx, bool server)
    : ctx_(ctx)
    , server_(server)
{
}

TlsContext::~TlsContext() { SSL_CTX_free(ctx_); } // 连接各自持有 SSL_CTX 的引用，所以它们还在用时这里只是减一

std::shared_ptr<const TlsContext> TlsContext::CreateServer(const TlsServerOptions& o, std::string& error)
{
    error.clear();
    auto ctx = NewCtx(o.min_version, o.max_version, error);
    if (!ctx)
        return nullptr;

    const Pem id = LoadIdentity(ctx.get(), o.cert_chain, o.private_key, error);
    if (id == Pem::Absent)
        error = "服务端必须配置 cert_chain 与 private_key";
    if (id != Pem::Ok)
        return nullptr;

    if (o.client_ca.configured())
    {
        std::string ca_pem;
        if (LoadPem(o.client_ca, "client_ca", ca_pem, error) != Pem::Ok)
            return nullptr;
        STACK_OF(X509_NAME)* names = sk_X509_NAME_new_null();
        const std::size_t n = AddCas(ctx.get(), ca_pem, names);
        if (n == 0)
        {
            sk_X509_NAME_pop_free(names, X509_NAME_free);
            error = "client_ca 里没有可解析的 PEM 证书";
            return nullptr;
        }
        SSL_CTX_set_client_CA_list(ctx.get(), names); // 取得 names 的所有权
        SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_PEER | (o.require_client_cert ? SSL_VERIFY_FAIL_IF_NO_PEER_CERT : 0), nullptr);
    }
    else if (o.require_client_cert)
    {
        error = "require_client_cert 需要配置 client_ca";
        return nullptr;
    }

    // 不存服务端会话：10 万连接不为"恢复"付内存。无状态的 session ticket 照常工作（实测 SSL_SESS_CACHE_OFF 下恢复 3000/3000 成功）。
    SSL_CTX_set_session_cache_mode(ctx.get(), SSL_SESS_CACHE_OFF);
    SSL_CTX_set_num_tickets(ctx.get(), 1);
    if (!o.session_tickets)
        SSL_CTX_set_options(ctx.get(), SSL_OP_NO_TICKET);

    return std::shared_ptr<const TlsContext>(new TlsContext(ctx.release(), /*server=*/true));
}

std::shared_ptr<const TlsContext> TlsContext::CreateClient(const TlsClientOptions& o, std::string& error)
{
    error.clear();
    if (o.verify_peer && o.server_name.empty())
    {
        error = "verify_peer 为真时必须配置 server_name（SNI 与主机名校验要用它）；只在测试里才关 verify_peer";
        return nullptr;
    }
    auto ctx = NewCtx(o.min_version, o.max_version, error);
    if (!ctx)
        return nullptr;

    if (LoadIdentity(ctx.get(), o.cert_chain, o.private_key, error) == Pem::Failed)
        return nullptr;

    if (o.verify_peer)
    {
        SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_PEER, nullptr);
        if (o.ca.configured())
        {
            std::string ca_pem;
            if (LoadPem(o.ca, "ca", ca_pem, error) != Pem::Ok)
                return nullptr;
            if (AddCas(ctx.get(), ca_pem, nullptr) == 0)
            {
                error = "ca 里没有可解析的 PEM 证书";
                return nullptr;
            }
        }
        else if (SSL_CTX_set_default_verify_paths(ctx.get()) != 1)
        {
            error = "加载系统 CA 目录失败：" + OpenSslError();
            return nullptr;
        }
    }
    else
    {
        SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_NONE, nullptr);
    }

    std::shared_ptr<TlsSessionBox> box;
    if (o.session_resumption)
    {
        box = std::make_shared<TlsSessionBox>();
        // 只在 ex_data 里留一份引用；NO_INTERNAL：不用 BoringSSL 自带的客户端会话缓存（它按服务端地址存，我们只要最近一张）。
        SSL_CTX_set_ex_data(ctx.get(), BoxIndex(), new std::shared_ptr<TlsSessionBox>(box));
        SSL_CTX_set_session_cache_mode(ctx.get(), SSL_SESS_CACHE_CLIENT | SSL_SESS_CACHE_NO_INTERNAL);
        SSL_CTX_sess_set_new_cb(ctx.get(), OnNewSession);
    }

    std::shared_ptr<TlsContext> self(new TlsContext(ctx.release(), /*server=*/false));
    self->server_name_ = o.server_name;
    self->verify_peer_ = o.verify_peer;
    self->sessions_ = std::move(box);
    return self;
}

std::unique_ptr<IStreamLayer> TlsContext::Create(LayerRole role) const
{
    if ((role == LayerRole::Server) != server_)
        LOG_FATAL("TlsContext 的角色与请求的层角色不一致（服务端上下文不能建客户端层，反之亦然）");

    SSL* ssl = SSL_new(ctx_);
    if (ssl == nullptr)
        LOG_FATAL("SSL_new 失败（内存不足？）：{}", OpenSslError());
    // 握手完成后释放握手期的配置与中间状态：每条空闲连接服务端一侧 3.9KB → 2.8KB（实测）。
    SSL_set_shed_handshake_config(ssl, 1);

    if (!server_)
    {
        if (!server_name_.empty())
        {
            std::string name = server_name_;
            if (name.size() >= 2 && name.front() == '[' && name.back() == ']') // "[::1]"
                name = name.substr(1, name.size() - 2);
            const bool is_ip = IsIpLiteral(name);
            if (!is_ip)
                SSL_set_tlsext_host_name(ssl, name.c_str()); // IP 字面量不发 SNI（RFC 6066）
            if (verify_peer_)
            {
                X509_VERIFY_PARAM* param = SSL_get0_param(ssl);
                X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
                if (is_ip)
                    X509_VERIFY_PARAM_set1_ip_asc(param, name.c_str());
                else
                    X509_VERIFY_PARAM_set1_host(param, name.c_str(), name.size());
            }
        }
        if (sessions_)
        {
            const std::lock_guard lk(sessions_->mu);
            if (sessions_->last != nullptr)
                SSL_set_session(ssl, sessions_->last); // 内部加一份引用
        }
    }
    return std::make_unique<tls::TlsLayer>(ssl, role);
}

TlsContextHolder::TlsContextHolder(std::shared_ptr<const TlsContext> initial)
    : current_(std::move(initial))
{
}

void TlsContextHolder::Reload(std::shared_ptr<const TlsContext> next) { current_.store(std::move(next)); }

std::unique_ptr<IStreamLayer> TlsContextHolder::Create(LayerRole role) const
{
    const std::shared_ptr<const TlsContext> ctx = current_.load(); // 拿一份引用：Reload 与 Create 并发时旧上下文不会在中途销毁
    return ctx->Create(role);
}

} // namespace breeze
