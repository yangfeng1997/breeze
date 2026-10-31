// 网络端点：TCP / UDP（IP 字面量 + 端口）、UDS 路径、UDS 抽象名。值类型，构造即校验，拿到手的一定合法。
// TCP 与 UDP 只是传输不同，地址的写法与校验完全一样；UDP 端点给数据报传输用（将来的 rudp），字节流的类不收它。
// 设计见 docs/网络库设计.md §4.2。

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace breeze
{

class Endpoint
{
public:
    enum class Kind : std::uint8_t
    {
        Tcp,
        Udp,
        Unix,
        UnixAbstract,
    };

    // 只接 IP 字面量（v4 / v6），不做 DNS：getaddrinfo 会阻塞，不能在 IO 线程跑，解析是调用方的事。
    // ip 规范化后保存（"0:0:0:0:0:0:0:1" 与 "::1" 相等）。
    static std::optional<Endpoint> Tcp(std::string_view ip, std::uint16_t port);
    // 同 Tcp：只接 IP 字面量，规范化后保存。
    static std::optional<Endpoint> Udp(std::string_view ip, std::uint16_t port);

    // 路径 / 抽象名都不能为空、不能含 '\0'、长度 ≤107（sockaddr_un.sun_path 共 108 字节，含结尾）。
    // Unix 路径必须是绝对路径：相对路径依赖进程工作目录，两端容易对不上。
    static std::optional<Endpoint> Unix(std::string_view path);
    static std::optional<Endpoint> UnixAbstract(std::string_view name);

    // tcp://1.2.3.4:80 | tcp://[::1]:80 | udp://1.2.3.4:80 | udp://[::1]:80 |
    // unix:///run/x.sock | unix-abstract://name；其它一律返回 nullopt。
    static std::optional<Endpoint> Parse(std::string_view text);

    Kind kind() const noexcept { return kind_; }
    // Tcp / Udp：规范化后的 IP 文本；Unix / UnixAbstract：路径 / 抽象名。
    const std::string& address() const noexcept { return address_; }
    // 仅 Tcp / Udp 有意义；0 表示由系统分配（bind 用）。
    std::uint16_t port() const noexcept { return port_; }
    // IP 类端点（Tcp / Udp）；否则是 UDS。
    bool IsIp() const noexcept { return kind_ == Kind::Tcp || kind_ == Kind::Udp; }
    bool IsIPv6() const noexcept { return IsIp() && address_.find(':') != std::string::npos; }

    // 与 Parse 互逆：Parse(ep.ToString()) == ep。
    std::string ToString() const;

    bool operator==(const Endpoint&) const = default;

private:
    Endpoint(Kind kind, std::string address, std::uint16_t port)
        : kind_(kind)
        , address_(std::move(address))
        , port_(port)
    {
    }

    Kind kind_;
    std::string address_;
    std::uint16_t port_;
};

} // namespace breeze
