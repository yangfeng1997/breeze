// WebSocket 帧头 / 掩码 / 关闭码：RFC 6455 §5.7 的样例、帧头回环、任意前缀必为 NeedMore、非法帧头一律 Error、
// 掩码与逐字节参考实现一致（含各种起始对齐与长度）、随机数据不崩（asan / ubsan 下跑）。
// 无测试框架依赖：失败即 abort。

#include "base/net/ws/internal/ws_frame.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <vector>

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

using namespace breeze::ws;

std::vector<std::byte> Bytes(std::initializer_list<int> v)
{
    std::vector<std::byte> out;
    for (const int x : v)
        out.push_back(static_cast<std::byte>(x));
    return out;
}

std::string Text(std::span<const std::byte> b) { return {reinterpret_cast<const char*>(b.data()), b.size()}; }

HeaderParse Parse(const std::vector<std::byte>& d, bool expect_masked, FrameHeader& h) { return ParseFrameHeader(d, expect_masked, h); }

// RFC 6455 §5.7：单帧未加掩码的文本 "Hello"
void TestRfcUnmaskedHello()
{
    const auto f = Bytes({0x81, 0x05, 0x48, 0x65, 0x6c, 0x6c, 0x6f});
    FrameHeader h;
    CHECK(Parse(f, false, h) == HeaderParse::Ok);
    CHECK(h.fin && h.opcode == Opcode::Text && !h.masked && h.payload_len == 5 && h.header_size == 2);
    CHECK(Text(std::span(f).subspan(h.header_size)) == "Hello");
}

// 单帧加掩码的文本 "Hello"
void TestRfcMaskedHello()
{
    auto f = Bytes({0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58});
    FrameHeader h;
    CHECK(Parse(f, true, h) == HeaderParse::Ok);
    CHECK(h.fin && h.opcode == Opcode::Text && h.masked && h.payload_len == 5 && h.header_size == 6);
    CHECK(h.mask_key == (std::array<std::byte, 4>{std::byte{0x37}, std::byte{0xfa}, std::byte{0x21}, std::byte{0x3d}}));
    const std::span<std::byte> payload = std::span(f).subspan(h.header_size);
    ApplyMask(payload, h.mask_key);
    CHECK(Text(payload) == "Hello");
}

// 分片的 "Hel" + "lo"；Ping / Pong 样例
void TestRfcFragmentsAndControl()
{
    FrameHeader h;
    CHECK(Parse(Bytes({0x01, 0x03, 0x48, 0x65, 0x6c}), false, h) == HeaderParse::Ok);
    CHECK(!h.fin && h.opcode == Opcode::Text && h.payload_len == 3);
    CHECK(Parse(Bytes({0x80, 0x02, 0x6c, 0x6f}), false, h) == HeaderParse::Ok);
    CHECK(h.fin && h.opcode == Opcode::Continuation && h.payload_len == 2);
    CHECK(Parse(Bytes({0x89, 0x05, 0x48, 0x65, 0x6c, 0x6c, 0x6f}), false, h) == HeaderParse::Ok);
    CHECK(h.fin && h.opcode == Opcode::Ping && h.payload_len == 5);
    CHECK(Parse(Bytes({0x8a, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58}), true, h) == HeaderParse::Ok);
    CHECK(h.fin && h.opcode == Opcode::Pong && h.masked && h.payload_len == 5);
}

// 256 字节二进制用 4 字节头，64KiB 用 10 字节头
void TestRfcLengthForms()
{
    FrameHeader h;
    CHECK(Parse(Bytes({0x82, 0x7E, 0x01, 0x00}), false, h) == HeaderParse::Ok);
    CHECK(h.opcode == Opcode::Binary && h.payload_len == 256 && h.header_size == 4);
    CHECK(Parse(Bytes({0x82, 0x7F, 0, 0, 0, 0, 0, 0x01, 0x00, 0x00}), false, h) == HeaderParse::Ok);
    CHECK(h.payload_len == 65536 && h.header_size == 10);
}

// 一个合法帧头的任何真前缀都是 NeedMore（不是 Error、也不会越界读）。
void TestEveryPrefixIsNeedMore()
{
    const std::uint64_t lens[] = {0, 1, 125, 126, 1000, 65535, 65536, 1u << 20, (std::uint64_t{1} << 32) + 5};
    for (const bool masked : {false, true})
        for (const std::uint64_t len : lens)
        {
            std::array<std::byte, 14> buf{};
            const std::array<std::byte, 4> key = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
            const std::size_t n = EncodeFrameHeader(true, Opcode::Binary, len, masked ? &key : nullptr, buf);
            for (std::size_t cut = 0; cut < n; ++cut)
            {
                FrameHeader h;
                CHECK(ParseFrameHeader(std::span(buf.data(), cut), masked, h) == HeaderParse::NeedMore);
            }
            FrameHeader h;
            CHECK(ParseFrameHeader(std::span(buf.data(), n), masked, h) == HeaderParse::Ok);
        }
}

// 编码再解析回到同样的字段；头长是 2 / 4 / 10（带掩码各加 4）。
void TestEncodeParseRoundTrip()
{
    const std::uint64_t lens[] = {0, 1, 124, 125, 126, 127, 65535, 65536, 1u << 24, (std::uint64_t{1} << 40) + 7, (std::uint64_t{1} << 63) - 1};
    const Opcode ops[] = {Opcode::Text, Opcode::Binary, Opcode::Continuation};
    for (const bool masked : {false, true})
        for (const bool fin : {false, true})
            for (const Opcode op : ops)
                for (const std::uint64_t len : lens)
                {
                    std::array<std::byte, 14> buf{};
                    const std::array<std::byte, 4> key = {std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}, std::byte{0xEF}};
                    const std::size_t n = EncodeFrameHeader(fin, op, len, masked ? &key : nullptr, buf);
                    const std::size_t base = len < 126 ? 2 : (len <= 0xFFFF ? 4 : 10);
                    CHECK(n == base + (masked ? 4 : 0));
                    FrameHeader h;
                    CHECK(ParseFrameHeader(std::span(buf.data(), n), masked, h) == HeaderParse::Ok);
                    CHECK(h.fin == fin && h.opcode == op && h.masked == masked && h.payload_len == len && h.header_size == n);
                    if (masked)
                        CHECK(h.mask_key == key);
                }
}

void TestControlFrames()
{
    // 控制帧：不分片、≤125，EncodeFrameHeader 写出的合法
    std::array<std::byte, 14> buf{};
    for (const Opcode op : {Opcode::Close, Opcode::Ping, Opcode::Pong})
    {
        const std::size_t n = EncodeFrameHeader(true, op, 125, nullptr, buf);
        FrameHeader h;
        CHECK(ParseFrameHeader(std::span(buf.data(), n), false, h) == HeaderParse::Ok && h.opcode == op);
        // 126 字节的控制帧非法（用 4 字节头写出来）
        const std::size_t n2 = EncodeFrameHeader(true, op, 126, nullptr, buf);
        CHECK(ParseFrameHeader(std::span(buf.data(), n2), false, h) == HeaderParse::Error);
        // 分片的控制帧非法
        const std::size_t n3 = EncodeFrameHeader(false, op, 3, nullptr, buf);
        CHECK(ParseFrameHeader(std::span(buf.data(), n3), false, h) == HeaderParse::Error);
    }
}

void TestInvalidHeaders()
{
    FrameHeader h;
    // RSV1..3
    for (const int rsv : {0x40, 0x20, 0x10})
        CHECK(Parse(Bytes({0x81 | rsv, 0x00}), false, h) == HeaderParse::Error);
    // 保留的 opcode：3..7、0xB..0xF
    for (int op = 0x3; op <= 0x7; ++op)
        CHECK(Parse(Bytes({0x80 | op, 0x00}), false, h) == HeaderParse::Error);
    for (int op = 0xB; op <= 0xF; ++op)
        CHECK(Parse(Bytes({0x80 | op, 0x00}), false, h) == HeaderParse::Error);
    // MASK 位与方向不符
    CHECK(Parse(Bytes({0x81, 0x80, 0, 0, 0, 0}), false, h) == HeaderParse::Error);
    CHECK(Parse(Bytes({0x81, 0x00}), true, h) == HeaderParse::Error);
    // 非最短的长度编码
    CHECK(Parse(Bytes({0x82, 0x7E, 0x00, 0x7D}), false, h) == HeaderParse::Error);                      // 125 用了 16 位
    CHECK(Parse(Bytes({0x82, 0x7E, 0x00, 0x00}), false, h) == HeaderParse::Error);                      // 0 用了 16 位
    CHECK(Parse(Bytes({0x82, 0x7F, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF}), false, h) == HeaderParse::Error);    // 65535 用了 64 位
    CHECK(Parse(Bytes({0x82, 0x7F, 0, 0, 0, 0, 0, 0, 0, 0x01}), false, h) == HeaderParse::Error);       // 1 用了 64 位
    // 64 位长度最高位为 1
    CHECK(Parse(Bytes({0x82, 0x7F, 0x80, 0, 0, 0, 0, 0, 0, 0}), false, h) == HeaderParse::Error);
    CHECK(Parse(Bytes({0x82, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}), false, h) == HeaderParse::Error);
    // 边界上合法的：126 与 65536 恰是各自形式的最小值
    CHECK(Parse(Bytes({0x82, 0x7E, 0x00, 0x7E}), false, h) == HeaderParse::Ok && h.payload_len == 126);
    CHECK(Parse(Bytes({0x82, 0x7F, 0, 0, 0, 0, 0, 0x01, 0x00, 0x00}), false, h) == HeaderParse::Ok && h.payload_len == 65536);
    CHECK(Parse(Bytes({0x82, 0x7E, 0xFF, 0xFF}), false, h) == HeaderParse::Ok && h.payload_len == 65535);
}

// 先判错、后判够不够：非法头在字节不全时也要尽早 Error 的，比如保留位（只需第一个字节）。
void TestEarlyError()
{
    FrameHeader h;
    CHECK(Parse(Bytes({0x71, 0x00}), false, h) == HeaderParse::Error); // 两个字节就够判 RSV
    CHECK(ParseFrameHeader(std::span<const std::byte>{}, false, h) == HeaderParse::NeedMore);
    CHECK(Parse(Bytes({0x81}), false, h) == HeaderParse::NeedMore);
}

void TestMaskMatchesReference()
{
    const std::array<std::byte, 4> key = {std::byte{0x37}, std::byte{0xFA}, std::byte{0x21}, std::byte{0x3D}};
    std::vector<std::byte> backing(8 + 200 + 8);
    for (std::size_t i = 0; i < backing.size(); ++i)
        backing[i] = static_cast<std::byte>(i * 31 + 7);
    for (std::size_t offset = 0; offset < 8; ++offset) // 起始地址的各种对齐
        for (std::size_t len = 0; len <= 100; ++len)
        {
            std::vector<std::byte> ref(backing.begin() + static_cast<std::ptrdiff_t>(offset),
                                       backing.begin() + static_cast<std::ptrdiff_t>(offset + len));
            for (std::size_t i = 0; i < len; ++i)
                ref[i] ^= key[i % 4];

            std::vector<std::byte> work = backing;
            ApplyMask(std::span(work).subspan(offset, len), key);
            CHECK(std::equal(ref.begin(), ref.end(), work.begin() + static_cast<std::ptrdiff_t>(offset)));
            // 两侧不能被碰
            CHECK(std::equal(backing.begin(), backing.begin() + static_cast<std::ptrdiff_t>(offset), work.begin()));
            CHECK(std::equal(backing.begin() + static_cast<std::ptrdiff_t>(offset + len), backing.end(),
                             work.begin() + static_cast<std::ptrdiff_t>(offset + len)));
            // 再来一次就还原
            ApplyMask(std::span(work).subspan(offset, len), key);
            CHECK(work == backing);
        }
}

void TestCloseCodes()
{
    for (const int c : {1000, 1001, 1002, 1003, 1007, 1008, 1009, 1010, 1011, 3000, 3999, 4000, 4999})
        CHECK(IsValidCloseCode(static_cast<std::uint16_t>(c)));
    for (const int c : {0, 1, 999, 1004, 1005, 1006, 1012, 1015, 1016, 1100, 2000, 2999, 5000, 65535})
        CHECK(!IsValidCloseCode(static_cast<std::uint16_t>(c)));
}

// 随机字节喂给解析器：不崩、不越界（asan / ubsan 下才有意义）；Ok 时 header_size 一定落在数据范围内且与长度形式一致。
void TestRandomBytesNeverCrash()
{
    std::uint64_t s = 88172645463325252ull; // xorshift64
    const auto next = [&s]
    {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    };
    for (int iter = 0; iter < 200000; ++iter)
    {
        std::vector<std::byte> d(next() % 16);
        for (auto& b : d)
            b = static_cast<std::byte>(next());
        for (const bool expect : {false, true})
        {
            FrameHeader h;
            if (ParseFrameHeader(d, expect, h) == HeaderParse::Ok)
            {
                CHECK(h.header_size >= 2 && h.header_size <= d.size() && h.header_size <= kMaxHeaderSize);
                CHECK(h.masked == expect);
                CHECK(h.payload_len < (std::uint64_t{1} << 63));
            }
        }
    }
}

} // namespace

int main()
{
    TestRfcUnmaskedHello();
    TestRfcMaskedHello();
    TestRfcFragmentsAndControl();
    TestRfcLengthForms();
    TestEveryPrefixIsNeedMore();
    TestEncodeParseRoundTrip();
    TestControlFrames();
    TestInvalidHeaders();
    TestEarlyError();
    TestMaskMatchesReference();
    TestCloseCodes();
    TestRandomBytesNeverCrash();
    std::puts("ws_frame_test: ok");
    return 0;
}
