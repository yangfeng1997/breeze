// rudp 数据报格式：编码与解析互逆、字节布局固定（线格式，改了就是协议不兼容）、解析严格、回应不放大。
// 无测试框架依赖：失败即 abort。

#include "base/net/rudp/rudp_protocol.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace breeze;

#define CHECK(...)                                                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(__VA_ARGS__))                                                                                            \
        {                                                                                                              \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #__VA_ARGS__);                       \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (0)

namespace
{

std::vector<std::byte> Bytes(std::initializer_list<int> v)
{
    std::vector<std::byte> out;
    for (const int b : v)
        out.push_back(static_cast<std::byte>(b));
    return out;
}

rudp::Token MakeToken()
{
    rudp::Token t;
    for (std::size_t i = 0; i < t.size(); ++i)
        t[i] = static_cast<std::byte>(0x10 + i);
    return t;
}

std::vector<std::byte> Copy(const rudp::Control& c) { return {c.view().begin(), c.view().end()}; }

// 线格式：逐字节固定。别处（比如别的语言写的客户端）按这个实现，改动就是不兼容。
void TestWireLayout()
{
    const auto hello = rudp::EncodeHello(0x04030201u, MakeToken());
    auto want = Bytes({0xA1, 1, 0x01, 0x02, 0x03, 0x04});
    for (int i = 0; i < 16; ++i)
        want.push_back(static_cast<std::byte>(0x10 + i));
    CHECK(Copy(hello) == want);

    CHECK(Copy(rudp::EncodeAccept(0x04030201u)) == Bytes({0xA2, 1, 0x01, 0x02, 0x03, 0x04}));
    CHECK(Copy(rudp::EncodeReject(rudp::RejectReason::Busy)) == Bytes({0xA3, 2}));
    CHECK(Copy(rudp::EncodePing(0x0A0B0C0Du)) == Bytes({0xA5, 0x0D, 0x0C, 0x0B, 0x0A}));
    CHECK(Copy(rudp::EncodePong(0x0A0B0C0Du)) == Bytes({0xA6, 0x0D, 0x0C, 0x0B, 0x0A}));
    CHECK(Copy(rudp::EncodeClose(0x0A0B0C0Du, 3)) == Bytes({0xA7, 0x0D, 0x0C, 0x0B, 0x0A, 3}));
    CHECK(rudp::DataHeader() == std::byte{0xA4});
}

void TestRoundTrip()
{
    {
        const auto c = rudp::EncodeHello(0xDEADBEEFu, MakeToken());
        const auto p = rudp::Parse(c.view());
        CHECK(p && p->type == rudp::PacketType::Hello && p->version == rudp::kVersion && p->conv == 0xDEADBEEFu &&
              p->token == MakeToken());
    }
    {
        const auto p = rudp::Parse(rudp::EncodeAccept(7).view());
        CHECK(p && p->type == rudp::PacketType::Accept && p->conv == 7 && p->version == rudp::kVersion);
    }
    {
        const auto p = rudp::Parse(rudp::EncodeReject(rudp::RejectReason::Filtered).view());
        CHECK(p && p->type == rudp::PacketType::Reject &&
              p->reason == static_cast<std::uint8_t>(rudp::RejectReason::Filtered));
    }
    {
        const auto ping = rudp::Parse(rudp::EncodePing(9).view());
        const auto pong = rudp::Parse(rudp::EncodePong(9).view());
        CHECK(ping && ping->type == rudp::PacketType::Ping && ping->conv == 9);
        CHECK(pong && pong->type == rudp::PacketType::Pong && pong->conv == 9);
    }
    {
        const auto p = rudp::Parse(rudp::EncodeClose(11, 4).view());
        CHECK(p && p->type == rudp::PacketType::Close && p->conv == 11 && p->reason == 4);
    }
}

// Data：包头之后是 KCP 段，body 指向入参里的同一块内存（不拷贝）。
void TestData()
{
    std::vector<std::byte> d(rudp::kDataPrefix + rudp::kKcpHeader + 5, std::byte{0x55});
    d[0] = rudp::DataHeader();
    const auto p = rudp::Parse(d);
    CHECK(p && p->type == rudp::PacketType::Data);
    CHECK(p->body.data() == d.data() + 1 && p->body.size() == d.size() - 1);

    // 刚好一个 KCP 头（没有载荷）是合法的（纯 ack / 探测）；少一个字节就不是
    d.resize(rudp::kDataPrefix + rudp::kKcpHeader);
    CHECK(rudp::Parse(d).has_value());
    d.resize(rudp::kDataPrefix + rudp::kKcpHeader - 1);
    CHECK(!rudp::Parse(d).has_value());
}

// 严格：长度差一个字节、类型不认识、高 4 位不对，都丢。
void TestParseIsStrict()
{
    CHECK(!rudp::Parse({}).has_value());
    for (const auto& c : {rudp::EncodeHello(1, MakeToken()),
                          rudp::EncodeAccept(1),
                          rudp::EncodeReject(rudp::RejectReason::Ticket),
                          rudp::EncodePing(1),
                          rudp::EncodePong(1),
                          rudp::EncodeClose(1, 0)})
    {
        auto v = Copy(c);
        CHECK(rudp::Parse(v).has_value());
        v.push_back(std::byte{0}); // 多一个字节
        CHECK(!rudp::Parse(v).has_value());
        v.pop_back();
        v.pop_back(); // 少一个字节
        CHECK(!rudp::Parse(v).has_value());
    }
    CHECK(!rudp::Parse(Bytes({0xA0, 0, 0, 0, 0, 0, 0})).has_value()); // 类型 0
    CHECK(!rudp::Parse(Bytes({0xA8, 0, 0, 0, 0, 0, 0})).has_value()); // 类型 8
    CHECK(!rudp::Parse(Bytes({0xAF, 0, 0, 0, 0, 0, 0})).has_value());
    // 高 4 位不是 0xA：不是本协议（别的东西打到这个端口）
    auto bad = Copy(rudp::EncodeAccept(1));
    bad[0] = std::byte{0xB2};
    CHECK(!rudp::Parse(bad).has_value());
    bad[0] = std::byte{0x02};
    CHECK(!rudp::Parse(bad).has_value());
}

// 服务端回应的控制包不比触发它的包大：Reject / Accept 回 Hello，Close 回最小的 Data / Ping。
void TestNoAmplification()
{
    const std::size_t hello = rudp::EncodeHello(1, MakeToken()).size;
    CHECK(rudp::EncodeReject(rudp::RejectReason::Ticket).size < hello);
    CHECK(rudp::EncodeAccept(1).size <= hello);
    const std::size_t min_data = rudp::kDataPrefix + rudp::kKcpHeader;
    CHECK(rudp::EncodeClose(1, 0).size < min_data);
    CHECK(rudp::EncodeClose(1, 0).size >
          rudp::EncodePing(1).size - 1); // Ping 是 5、Close 是 6：回 Ping 略大一字节，已知并接受
}

void TestPeekKcpConv()
{
    CHECK(rudp::PeekKcpConv(Bytes({0x78, 0x56, 0x34, 0x12, 0xFF})) == 0x12345678u);
    CHECK(rudp::PeekKcpConv(Bytes({0x78, 0x56, 0x34, 0x12})) == 0x12345678u); // 恰好 4 字节
    CHECK(rudp::PeekKcpConv(Bytes({1, 2, 3})) == 0);
    CHECK(rudp::PeekKcpConv({}) == 0);
}

void TestMaxMessage()
{
    // 每段 mss = mtu - 1（包头）- 24（KCP 头），一条消息最多 127 段
    CHECK(rudp::MaxMessageForMtu(1400) == 127u * (1400 - 1 - 24));
    CHECK(rudp::MaxMessageForMtu(rudp::kMinMtu) == 127u * (576 - 25));
    CHECK(rudp::MaxMessageForMtu(1500) > rudp::MaxMessageForMtu(1400));
}

} // namespace

int main()
{
    TestWireLayout();
    TestRoundTrip();
    TestData();
    TestParseIsStrict();
    TestNoAmplification();
    TestPeekKcpConv();
    TestMaxMessage();
    std::puts("rudp_protocol_test: ok");
    return 0;
}
