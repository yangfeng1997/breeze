// WebSocket 测试的公共件（只给 *_test.cpp 用，不进库）：独立手写的帧构造 / 解析、升级请求，以及一个直接收发原始字节的
// 对照客户端 WsRawClient。**刻意不复用被测代码的 ws_frame / http_upgrade**：测试里的帧编码是另一份实现，
// 两份对不上才说明有一边错了。
//
// WsRawClient 可以选择穿过假的字节流层（fake_layer_test_util.h：HELO / OK!! 握手、之后逐字节异或），用来模拟 wss 的叠加方式。

#pragma once

#include <netinet/in.h>
#include <netinet/tcp.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "base/net/testing/fake_layer_test_util.h"
#include "base/net/testing/net_test_util.h"

namespace breeze::testing
{

inline constexpr const char* kWsKey = "dGhlIHNhbXBsZSBub25jZQ=="; // RFC 6455 §1.3 的例子
inline constexpr const char* kWs101 =
    "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
    "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n";

// 一个完整的帧。mask = true 是客户端发给服务端的样子（key 是掩码键，大端展开）。
inline std::string WsFrame(int opcode, std::string_view payload, bool fin = true, bool mask = true, std::uint32_t key = 0x12345678)
{
    std::string f;
    f.push_back(static_cast<char>((fin ? 0x80 : 0) | opcode));
    const std::size_t n = payload.size();
    const int m = mask ? 0x80 : 0;
    if (n < 126)
    {
        f.push_back(static_cast<char>(m | static_cast<int>(n)));
    }
    else if (n <= 0xFFFF)
    {
        f.push_back(static_cast<char>(m | 126));
        f.push_back(static_cast<char>(n >> 8));
        f.push_back(static_cast<char>(n));
    }
    else
    {
        f.push_back(static_cast<char>(m | 127));
        for (int i = 7; i >= 0; --i)
            f.push_back(static_cast<char>((static_cast<std::uint64_t>(n) >> (8 * i)) & 0xFF));
    }
    if (mask)
    {
        const unsigned char k[4] = {static_cast<unsigned char>(key >> 24), static_cast<unsigned char>(key >> 16),
                                    static_cast<unsigned char>(key >> 8), static_cast<unsigned char>(key)};
        f.append(reinterpret_cast<const char*>(k), 4);
        for (std::size_t i = 0; i < n; ++i)
            f.push_back(static_cast<char>(static_cast<unsigned char>(payload[i]) ^ k[i % 4]));
    }
    else
    {
        f.append(payload);
    }
    return f;
}

// 只有帧头、声称 payload 有 len 字节（测"帧头一到就判长度上限"）。
inline std::string WsHeaderOnly(int opcode, std::uint64_t len, bool mask = true)
{
    std::string f;
    f.push_back(static_cast<char>(0x80 | opcode));
    const int m = mask ? 0x80 : 0;
    if (len < 126)
    {
        f.push_back(static_cast<char>(m | static_cast<int>(len)));
    }
    else if (len <= 0xFFFF)
    {
        f.push_back(static_cast<char>(m | 126));
        f.push_back(static_cast<char>(len >> 8));
        f.push_back(static_cast<char>(len));
    }
    else
    {
        f.push_back(static_cast<char>(m | 127));
        for (int i = 7; i >= 0; --i)
            f.push_back(static_cast<char>((len >> (8 * i)) & 0xFF));
    }
    if (mask)
        f.append("\x01\x02\x03\x04", 4);
    return f;
}

inline std::string WsCloseBody(int code, std::string_view reason = "")
{
    std::string b;
    b.push_back(static_cast<char>(code >> 8));
    b.push_back(static_cast<char>(code));
    b.append(reason);
    return b;
}

struct WsOutFrame
{
    int opcode = 0;
    bool fin = false;
    std::string payload;
    bool operator==(const WsOutFrame&) const = default;
};

// 从 s 开头解析一帧。数据不够返回 false；解析成功时 consumed 是占用的字节数。
// expect_masked：服务端写的帧不加掩码（false），客户端写的必须加（true）。不合规直接 abort。
inline bool WsTryParseFrame(std::string_view s, bool expect_masked, WsOutFrame& out, std::size_t& consumed)
{
    const auto u8 = [&](std::size_t k) { return static_cast<unsigned>(static_cast<unsigned char>(s[k])); };
    if (s.size() < 2)
        return false;
    const unsigned b0 = u8(0);
    const unsigned b1 = u8(1);
    std::size_t i = 2;
    CHECK((b0 & 0x70) == 0);
    const bool masked = (b1 & 0x80) != 0;
    CHECK(masked == expect_masked);
    std::uint64_t len = b1 & 0x7F;
    if (len == 126)
    {
        if (s.size() < i + 2)
            return false;
        len = (u8(i) << 8) | u8(i + 1);
        i += 2;
        CHECK(len >= 126);
    }
    else if (len == 127)
    {
        if (s.size() < i + 8)
            return false;
        len = 0;
        for (std::size_t k = 0; k < 8; ++k)
            len = (len << 8) | u8(i + k);
        i += 8;
        CHECK(len > 0xFFFF);
    }
    unsigned char key[4] = {};
    if (masked)
    {
        if (s.size() < i + 4)
            return false;
        std::memcpy(key, s.data() + i, 4);
        i += 4;
    }
    if (s.size() - i < len)
        return false;
    std::string p(s.substr(i, static_cast<std::size_t>(len)));
    if (masked)
        for (std::size_t k = 0; k < p.size(); ++k)
            p[k] = static_cast<char>(static_cast<unsigned char>(p[k]) ^ key[k % 4]);
    consumed = i + static_cast<std::size_t>(len);
    out = {static_cast<int>(b0 & 0x0F), (b0 & 0x80) != 0, std::move(p)};
    return true;
}

// 解析一串完整的帧（不能有残余字节）。
inline std::vector<WsOutFrame> WsParseFrames(std::string_view s, bool expect_masked)
{
    std::vector<WsOutFrame> out;
    while (!s.empty())
    {
        WsOutFrame f;
        std::size_t used = 0;
        CHECK(WsTryParseFrame(s, expect_masked, f, used));
        out.push_back(std::move(f));
        s.remove_prefix(used);
    }
    return out;
}

inline std::string WsHandshakeRequest(std::string_view path = "/ws", const std::string& extra = "", const std::string& key = kWsKey)
{
    return "GET " + std::string(path) +
           " HTTP/1.1\r\nHost: example.com\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
           "\r\nSec-WebSocket-Version: 13\r\n" + extra + "\r\n";
}

// 直接收发原始字节的对照客户端：自己拼请求、自己编码帧、自己解析应答与帧。所有等待带超时。
class WsRawClient
{
public:
    // fake_layer：先完成假层的握手（HELO / OK!!），之后线上的每个字节都与 0x5A 异或。
    explicit WsRawClient(const Endpoint& ep, bool fake_layer = false)
        : c_(ep)
        , fake_layer_(fake_layer)
    {
        const int one = 1;
        CHECK(::setsockopt(c_.fd(), IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) == 0); // 小块发送不被合并，切分才真的到达服务端
        if (fake_layer_)
        {
            c_.SendAll(std::as_bytes(std::span(std::string_view("HELO"))));
            std::string hs;
            while (hs.size() < 4)
            {
                CHECK(c_.Wait(POLLIN, kWait));
                char b[4];
                const ssize_t n = ::recv(c_.fd(), b, 4 - hs.size(), 0);
                CHECK(n > 0);
                hs.append(b, static_cast<std::size_t>(n));
            }
            CHECK(hs == "OK!!");
        }
    }

    void Send(std::string_view s)
    {
        const std::string t = fake_layer_ ? FakeXor(std::string(s)) : std::string(s);
        c_.SendAll(std::as_bytes(std::span(t)));
    }

    // 一次几个字节地发（配合 NODELAY，让服务端真的看到被切碎的输入），块间稍等。
    void SendTrickled(std::string_view s, std::size_t chunk)
    {
        for (std::size_t i = 0; i < s.size(); i += chunk)
        {
            Send(s.substr(i, chunk));
            std::this_thread::sleep_for(200us);
        }
    }

    // 读到 HTTP 头结束（含空行）；之后多读到的字节留在缓冲里（紧跟的帧）。
    std::string ReadHead()
    {
        for (;;)
        {
            const std::size_t p = buf_.find("\r\n\r\n");
            if (p != std::string::npos)
            {
                std::string head = buf_.substr(0, p + 4);
                buf_.erase(0, p + 4);
                return head;
            }
            CHECK(Fill());
        }
    }

    WsOutFrame ReadFrame()
    {
        for (;;)
        {
            WsOutFrame f;
            std::size_t used = 0;
            if (WsTryParseFrame(buf_, /*expect_masked=*/false, f, used))
            {
                buf_.erase(0, used);
                return f;
            }
            CHECK(Fill());
        }
    }

    // 读到对端关闭（EOF / RST），返回此前没被取走的全部字节。
    std::string ReadToEof()
    {
        while (Fill())
        {
        }
        std::string r = std::move(buf_);
        buf_.clear();
        return r;
    }

    void Close() { c_.Close(); }
    TestClient& raw() { return c_; }

private:
    // 读一批；EOF / RST 返回 false。
    bool Fill()
    {
        CHECK(c_.Wait(POLLIN, kWait));
        char tmp[64 * 1024];
        const ssize_t n = ::recv(c_.fd(), tmp, sizeof(tmp), 0);
        if (n > 0)
        {
            std::string got(tmp, static_cast<std::size_t>(n));
            buf_ += fake_layer_ ? FakeXor(std::move(got)) : std::move(got);
            return true;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
            return true;
        return false;
    }

    TestClient c_;
    bool fake_layer_;
    std::string buf_;
};

} // namespace breeze::testing
