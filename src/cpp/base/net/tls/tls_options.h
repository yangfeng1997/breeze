// TLS 的配置。设计见方案 §4（docs/网络库设计.md 的 TLS 一节）。
//
// 证书与私钥都可以给**文件路径**或**内存里的 PEM 文本**（二选一，pem 优先）：密钥管理系统 / 测试用内存，运维用文件。
// 密钥内容不进配置文件、不进日志。
//
// 选型建议（写进文档，不是库强制的）：服务端证书用 **ECDSA P-256**——一次完整握手服务端约 61µs（RSA-2048 约 390µs），
// 网关重启后的重连风暴里差一个数量级。服务端 groups 默认只开 X25519 + P-256（BoringSSL 的默认会选后量子混合的
// X25519MLKEM768，服务端多约 40% CPU、握手包更大，游戏流量没有"先抓包后解密"的价值）。

#pragma once

#include <string>

namespace breeze
{

enum class TlsVersion : unsigned char
{
    Tls12,
    Tls13,
};

// 一份 PEM 的来源。pem 非空就用它，否则读 file；都空 = 没有配置。
struct TlsPemSource
{
    std::string file;
    std::string pem;

    bool configured() const noexcept { return !file.empty() || !pem.empty(); }
};

struct TlsServerOptions
{
    TlsPemSource cert_chain;  // 叶证书在前，后接中间证书
    TlsPemSource private_key; // 与叶证书配对；不能带口令

    // 校验客户端证书（mTLS）用的 CA。空 = 不要求客户端证书。
    TlsPemSource client_ca;
    // client_ca 非空时：true = 客户端不带证书就拒绝；false = 带了就校验，不带也放行（业务自己再判断）。
    bool require_client_cert = false;

    TlsVersion min_version = TlsVersion::Tls12;
    TlsVersion max_version = TlsVersion::Tls13; // 想固定在 1.2 才改；测试里用来强制走 1.2 的握手
    // 发 session ticket（无状态恢复，服务端不存会话）：重连风暴里对 ECDSA 省约 20%、对 RSA 省约 88% 的服务端 CPU。
    bool session_tickets = true;
};

struct TlsClientOptions
{
    // SNI 与证书主机名校验。写 IP 字面量时按 IP 校验、不发 SNI。
    // verify_peer 为真时必须填：不校验主机名的"校验"形同虚设，宁可创建失败。
    std::string server_name;
    // false 只给测试与内网自签用：不校验服务端证书，也就没有任何中间人防护。
    bool verify_peer = true;
    // 信任的 CA。空 = 系统 CA 目录。
    TlsPemSource ca;

    // mTLS：客户端证书与私钥，可选（两个一起配）。
    TlsPemSource cert_chain;
    TlsPemSource private_key;

    TlsVersion min_version = TlsVersion::Tls12;
    TlsVersion max_version = TlsVersion::Tls13;
    // 记住最近一次服务端发来的 session ticket，下次连接用它恢复（一个 TlsContext 对应一个服务端）。
    bool session_resumption = true;
};

} // namespace breeze
