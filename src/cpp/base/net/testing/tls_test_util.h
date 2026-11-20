// TLS 测试的公共件（只给 *_test.cpp 用，不进库）：运行时用 BoringSSL 生成各种测试证书（ECDSA / RSA、自签、CA 签发、
// 带 SAN、已过期、客户端证书），以及把两个 IStreamLayer 在内存里互相喂的 LayerPair。
// 不依赖任何固定的证书文件，所以过期、主机名不对、CA 不对这类负面场景都能按需造出来。
//
// 链接要求：测试要同时链 breeze_net_tls 与 OpenSSL::SSL / OpenSSL::Crypto（本头直接用 BoringSSL 的头）。

#pragma once

#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/ec_key.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/nid.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "base/net/stream/stream_layer_interface.h"
#include "base/net/testing/net_test_util.h"
#include "base/net/tls/tls_context.h"
#include "base/net/tls/tls_options.h"

// CertSpec 只填用得上的字段（指定初始化跳过中间的字段）：这里不报 clang 的 missing-designated-field-initializers。
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif

namespace breeze::testing
{

struct TestCert
{
    std::string cert_pem;
    std::string key_pem;
};

struct CertSpec
{
    std::string cn = "localhost";
    std::vector<std::string> dns;      // SAN: DNS
    std::vector<std::string> ips;      // SAN: IP
    bool is_ca = false;
    const TestCert* issuer = nullptr;  // 空 = 自签
    long not_before_sec = -3600;       // 相对现在
    long not_after_sec = 3600L * 24 * 365;
    bool rsa = false;                  // 默认 ECDSA P-256
};

namespace detail
{

inline std::string PemOf(bool key, void* obj)
{
    bssl::UniquePtr<BIO> bio(BIO_new(BIO_s_mem()));
    if (key)
        PEM_write_bio_PrivateKey(bio.get(), static_cast<EVP_PKEY*>(obj), nullptr, nullptr, 0, nullptr, nullptr);
    else
        PEM_write_bio_X509(bio.get(), static_cast<X509*>(obj));
    const std::uint8_t* p = nullptr;
    std::size_t n = 0;
    CHECK(BIO_mem_contents(bio.get(), &p, &n) == 1);
    return std::string(reinterpret_cast<const char*>(p), n);
}

inline bssl::UniquePtr<X509> ParseCert(const std::string& pem)
{
    bssl::UniquePtr<BIO> bio(BIO_new_mem_buf(pem.data(), static_cast<ossl_ssize_t>(pem.size())));
    return bssl::UniquePtr<X509>(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr));
}

inline bssl::UniquePtr<EVP_PKEY> ParseKey(const std::string& pem)
{
    bssl::UniquePtr<BIO> bio(BIO_new_mem_buf(pem.data(), static_cast<ossl_ssize_t>(pem.size())));
    return bssl::UniquePtr<EVP_PKEY>(PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr));
}

// 这版 BoringSSL 没有按字符串配置扩展的 X509V3_EXT_conf_nid（只有声明），所以用结构化的 API 构造。
inline void AddBasicConstraints(X509* x, bool ca)
{
    bssl::UniquePtr<BASIC_CONSTRAINTS> bc(BASIC_CONSTRAINTS_new());
    CHECK(bc != nullptr);
    bc->ca = ca ? 0xFF : 0;
    CHECK(X509_add1_ext_i2d(x, NID_basic_constraints, bc.get(), /*crit=*/ca ? 1 : 0, X509V3_ADD_DEFAULT) == 1);
}

inline void AddSubjectAltNames(X509* x, const std::vector<std::string>& dns, const std::vector<std::string>& ips)
{
    bssl::UniquePtr<GENERAL_NAMES> gens(GENERAL_NAMES_new());
    CHECK(gens != nullptr);
    for (const std::string& d : dns)
    {
        GENERAL_NAME* g = GENERAL_NAME_new();
        ASN1_IA5STRING* v = ASN1_IA5STRING_new();
        CHECK(g != nullptr && v != nullptr && ASN1_STRING_set(v, d.data(), static_cast<ossl_ssize_t>(d.size())) == 1);
        GENERAL_NAME_set0_value(g, GEN_DNS, v);
        CHECK(sk_GENERAL_NAME_push(gens.get(), g) != 0);
    }
    for (const std::string& ip : ips)
    {
        GENERAL_NAME* g = GENERAL_NAME_new();
        ASN1_OCTET_STRING* v = a2i_IPADDRESS(ip.c_str());
        CHECK(g != nullptr && v != nullptr);
        GENERAL_NAME_set0_value(g, GEN_IPADD, v);
        CHECK(sk_GENERAL_NAME_push(gens.get(), g) != 0);
    }
    CHECK(X509_add1_ext_i2d(x, NID_subject_alt_name, gens.get(), 0, X509V3_ADD_DEFAULT) == 1);
}

} // namespace detail

inline TestCert MakeCert(const CertSpec& s)
{
    static std::atomic<long> serial{1000};

    bssl::UniquePtr<EVP_PKEY> pkey(EVP_PKEY_new());
    if (s.rsa)
    {
        bssl::UniquePtr<RSA> rsa(RSA_new());
        bssl::UniquePtr<BIGNUM> e(BN_new());
        BN_set_word(e.get(), RSA_F4);
        CHECK(RSA_generate_key_ex(rsa.get(), 2048, e.get(), nullptr) == 1);
        CHECK(EVP_PKEY_assign_RSA(pkey.get(), rsa.release()) == 1);
    }
    else
    {
        bssl::UniquePtr<EC_KEY> ec(EC_KEY_new_by_curve_name(NID_X9_62_prime256v1));
        CHECK(EC_KEY_generate_key(ec.get()) == 1);
        CHECK(EVP_PKEY_assign_EC_KEY(pkey.get(), ec.release()) == 1);
    }

    bssl::UniquePtr<X509> x(X509_new());
    X509_set_version(x.get(), 2);
    ASN1_INTEGER_set(X509_get_serialNumber(x.get()), serial.fetch_add(1));
    X509_gmtime_adj(X509_getm_notBefore(x.get()), s.not_before_sec);
    X509_gmtime_adj(X509_getm_notAfter(x.get()), s.not_after_sec);
    X509_set_pubkey(x.get(), pkey.get());
    X509_NAME* name = X509_get_subject_name(x.get());
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>(s.cn.c_str()), -1, -1, 0);

    bssl::UniquePtr<X509> issuer_cert;
    bssl::UniquePtr<EVP_PKEY> issuer_key;
    if (s.issuer != nullptr)
    {
        issuer_cert = detail::ParseCert(s.issuer->cert_pem);
        issuer_key = detail::ParseKey(s.issuer->key_pem);
        CHECK(issuer_cert && issuer_key);
        X509_set_issuer_name(x.get(), X509_get_subject_name(issuer_cert.get()));
    }
    else
    {
        X509_set_issuer_name(x.get(), name);
    }

    detail::AddBasicConstraints(x.get(), s.is_ca);
    if (!s.dns.empty() || !s.ips.empty())
        detail::AddSubjectAltNames(x.get(), s.dns, s.ips);

    CHECK(X509_sign(x.get(), issuer_key ? issuer_key.get() : pkey.get(), EVP_sha256()) > 0);
    return {detail::PemOf(false, x.get()), detail::PemOf(true, pkey.get())};
}

// ---- 一套 PKI 与创建上下文的小帮手 ----

// 根 CA、由它签发的服务端 / 客户端证书；另一个无关的 CA 用来造"不被信任"的场景。全测试共用一套（生成证书要几十毫秒）。
struct TestPki
{
    TestCert ca = MakeCert({.cn = "Test CA", .is_ca = true});
    TestCert other_ca = MakeCert({.cn = "Other CA", .is_ca = true});
    TestCert server = MakeCert({.cn = "server.test", .dns = {"server.test", "alt.test"}, .ips = {"127.0.0.1", "::1"}, .issuer = &ca});
    TestCert client = MakeCert({.cn = "client-1", .issuer = &ca});
};

inline const TestPki& Pki()
{
    static const TestPki pki;
    return pki;
}

inline TlsServerOptions ServerOpts(const TestCert& c)
{
    TlsServerOptions o;
    o.cert_chain.pem = c.cert_pem;
    o.private_key.pem = c.key_pem;
    return o;
}

inline TlsClientOptions ClientOpts(const TestCert& ca, std::string name = "server.test")
{
    TlsClientOptions o;
    o.server_name = std::move(name);
    o.ca.pem = ca.cert_pem;
    return o;
}

inline std::shared_ptr<const TlsContext> MustServer(const TlsServerOptions& o)
{
    std::string err;
    auto c = TlsContext::CreateServer(o, err);
    if (!c)
        std::fprintf(stderr, "CreateServer: %s\n", err.c_str());
    CHECK(c != nullptr);
    return c;
}

inline std::shared_ptr<const TlsContext> MustClient(const TlsClientOptions& o)
{
    std::string err;
    auto c = TlsContext::CreateClient(o, err);
    if (!c)
        std::fprintf(stderr, "CreateClient: %s\n", err.c_str());
    CHECK(c != nullptr);
    return c;
}

// 一个一次性的 TCP 中继：接一条连接、转发到目标，把两个方向的字节都抓下来（证明线上没有明文时用）。
class Relay
{
public:
    explicit Relay(const Endpoint& target)
        : target_(target)
    {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        CHECK(listen_fd_ >= 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        CHECK(::bind(listen_fd_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
        CHECK(::listen(listen_fd_, 4) == 0);
        socklen_t len = sizeof(a);
        CHECK(::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&a), &len) == 0);
        port_ = ntohs(a.sin_port);
        thread_ = std::thread([this] { Run(); });
    }
    ~Relay()
    {
        stop_.store(true);
        thread_.join();
        ::close(listen_fd_);
    }
    Endpoint endpoint() const { return *Endpoint::Tcp("127.0.0.1", port_); }
    std::string captured() const
    {
        const std::lock_guard lk(mu_);
        return captured_;
    }

private:
    void Run()
    {
        pollfd lp{listen_fd_, POLLIN, 0};
        int a = -1;
        while (!stop_.load() && a < 0)
            if (::poll(&lp, 1, 50) > 0)
                a = ::accept(listen_fd_, nullptr, nullptr);
        if (a < 0)
            return;
        const int b = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in t{};
        t.sin_family = AF_INET;
        t.sin_port = htons(target_.port());
        ::inet_pton(AF_INET, target_.address().c_str(), &t.sin_addr);
        CHECK(::connect(b, reinterpret_cast<sockaddr*>(&t), sizeof(t)) == 0);
        char buf[64 * 1024];
        while (!stop_.load())
        {
            pollfd fds[2] = {{a, POLLIN, 0}, {b, POLLIN, 0}};
            if (::poll(fds, 2, 50) <= 0)
                continue;
            bool eof = false;
            for (int i = 0; i < 2; ++i)
            {
                if (!(fds[i].revents & (POLLIN | POLLHUP)))
                    continue;
                const ssize_t n = ::recv(fds[i].fd, buf, sizeof(buf), 0);
                if (n <= 0)
                {
                    eof = true;
                    continue;
                }
                {
                    const std::lock_guard lk(mu_);
                    captured_.append(buf, static_cast<std::size_t>(n));
                }
                const int other = i == 0 ? b : a;
                std::size_t off = 0;
                while (off < static_cast<std::size_t>(n))
                {
                    const ssize_t w = ::send(other, buf + off, static_cast<std::size_t>(n) - off, MSG_NOSIGNAL);
                    if (w <= 0)
                    {
                        eof = true;
                        break;
                    }
                    off += static_cast<std::size_t>(w);
                }
            }
            if (eof)
                break;
        }
        ::close(a);
        ::close(b);
    }

    Endpoint target_;
    int listen_fd_ = -1;
    std::uint16_t port_ = 0;
    std::atomic<bool> stop_{false};
    mutable std::mutex mu_;
    std::string captured_;
    std::thread thread_;
};



// ---- 两个层在内存里互相喂 ----

struct LayerEnd
{
    std::unique_ptr<IStreamLayer> layer;
    std::string plain;               // 收到的明文
    int handshake_done_count = 0;    // 返回 HandshakeDone 的次数（必须恰好一次）
    bool peer_shutdown = false;
    bool errored = false;
    bool started = false;
    bool plain_seen_before_done = false; // 先交付了明文、之后才报 HandshakeDone（层契约允许，连接据此保证 OnConnected 先于消息）

    void Apply(LayerStatus st)
    {
        switch (st)
        {
            case LayerStatus::Ok:
                break;
            case LayerStatus::HandshakeDone:
                ++handshake_done_count;
                break;
            case LayerStatus::PeerShutdown:
                peer_shutdown = true;
                break;
            case LayerStatus::Error:
                errored = true;
                break;
        }
    }
    bool done() const { return handshake_done_count > 0 || plain_seen_before_done; }

    void Start()
    {
        started = true;
        Apply(layer->Start());
    }

    // 线上字节进来：按 chunk 切分（0 = 一次给完）。返回是否出错。
    void Feed(std::span<const std::byte> wire, std::size_t chunk = 0)
    {
        std::vector<std::byte> copy(wire.begin(), wire.end()); // Unwrap 可以原地改写
        std::size_t pos = 0;
        while (pos < copy.size() && !errored)
        {
            const std::size_t n = chunk == 0 ? copy.size() - pos : std::min(chunk, copy.size() - pos);
            Apply(layer->Unwrap(std::span<std::byte>(copy).subspan(pos, n),
                                [this](std::span<std::byte> p)
                                {
                                    plain.append(reinterpret_cast<const char*>(p.data()), p.size());
                                    if (handshake_done_count == 0)
                                        plain_seen_before_done = true; // 先交付了明文、之后才报 HandshakeDone
                                    return true;
                                }));
            pos += n;
        }
    }

    // 把层里待发的线上字节取走。
    std::vector<std::byte> TakeWire()
    {
        const auto w = layer->PendingWire();
        std::vector<std::byte> out(w.begin(), w.end());
        layer->ConsumeWire(out.size());
        return out;
    }

    // 明文 → 线上字节（按 kLayerMaxWrapBytes 切块）。
    bool Send(std::string_view s)
    {
        for (std::size_t i = 0; i < s.size(); i += kLayerMaxWrapBytes)
        {
            const std::size_t n = std::min(kLayerMaxWrapBytes, s.size() - i);
            if (layer->Wrap(std::as_bytes(std::span(s.data() + i, n))) == LayerStatus::Error)
            {
                errored = true;
                return false;
            }
        }
        return true;
    }
};

struct LayerPair
{
    LayerEnd client;
    LayerEnd server;

    LayerPair(std::unique_ptr<IStreamLayer> c, std::unique_ptr<IStreamLayer> s)
    {
        client.layer = std::move(c);
        server.layer = std::move(s);
    }

    // 一轮：客户端的待发字节喂给服务端、服务端的喂给客户端。返回这一轮是否有字节流动。
    bool Pump(std::size_t chunk = 0)
    {
        if (!client.started)
            client.Start();
        bool moved = false;
        auto c2s = client.TakeWire();
        if (!c2s.empty())
        {
            moved = true;
            server.Feed(c2s, chunk);
        }
        auto s2c = server.TakeWire();
        if (!s2c.empty())
        {
            moved = true;
            client.Feed(s2c, chunk);
        }
        return moved;
    }

    // 泵到没有字节流动（或任一端出错）为止。
    void Run(std::size_t chunk = 0)
    {
        for (int i = 0; i < 1000; ++i)
        {
            if (!Pump(chunk))
                return;
            if (client.errored || server.errored)
            {
                return;
            }
        }
        CHECK(false && "LayerPair::Run：1000 轮还没停，握手死循环？");
    }
};

} // namespace breeze::testing
