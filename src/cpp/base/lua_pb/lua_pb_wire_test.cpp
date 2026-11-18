// wire 层：varint / zigzag / 定长 / Reader 边界 / Writer 回填。无测试框架，失败即 abort。

#include "base/lua_pb/wire.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace breeze::lua_pb;

#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                              \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (0)

namespace
{

// ---- 编译期用例：纯计算部分必须能 constexpr 求值 ----
constexpr bool ConstexprCases()
{
    std::byte buf[16]{};
    const std::byte* end = WriteVarint(buf, 300); // 0xAC 0x02
    if (end - buf != 2)
        return false;
    if (std::to_integer<std::uint8_t>(buf[0]) != 0xAC || std::to_integer<std::uint8_t>(buf[1]) != 0x02)
        return false;

    Reader r{std::span<const std::byte>{buf, 2}};
    std::uint64_t v = 0;
    if (!r.ReadVarint(v) || v != 300)
        return false;

    // zigzag 往返
    if (ZigZagDecode(ZigZagEncode(-1)) != -1 || ZigZagDecode(ZigZagEncode(1)) != 1)
        return false;
    if (ZigZagEncode(0) != 0 || ZigZagEncode(-1) != 1 || ZigZagEncode(1) != 2)
        return false;
    if (ZigZagEncode(INT64_MIN) != 0xFFFFFFFFFFFFFFFFull)
        return false;

    // 长度
    if (VarintSize(0) != 1 || VarintSize(127) != 1 || VarintSize(128) != 2 || VarintSize(16383) != 2 ||
        VarintSize(16384) != 3)
        return false;
    if (VarintSize(UINT64_MAX) != kMaxVarintBytes)
        return false;

    // tag
    if (TagFieldNumber(MakeTag(5, WireType::kLen)) != 5 || TagWireType(MakeTag(5, WireType::kLen)) != WireType::kLen)
        return false;
    return true;
}

static_assert(ConstexprCases());

void TestVarintBoundaries()
{
    const std::uint64_t values[] = {0,          1,          127,         128,        300,
                                    16383,      16384,      1u << 21,    UINT32_MAX, 1ull << 35,
                                    UINT64_MAX, UINT64_MAX / 2, 0x7FFFFFFFFFFFFFFFull};
    for (std::uint64_t v : values)
    {
        std::byte buf[16]{};
        const std::byte* end = WriteVarint(buf, v);
        CHECK(static_cast<std::size_t>(end - buf) == VarintSize(v));
        Reader r{std::span<const std::byte>{buf, static_cast<std::size_t>(end - buf)}};
        std::uint64_t got = 0;
        CHECK(r.ReadVarint(got));
        CHECK(got == v);
        CHECK(r.Empty());
    }
}

void TestVarintHostile()
{
    // 10 字节内合法：0xFFFFFFFFFFFFFFFF = 9 个 0xFF + 0x01
    {
        std::vector<std::byte> b(10, std::byte{0xFF});
        b[9] = std::byte{0x01};
        Reader r{b};
        std::uint64_t v = 0;
        CHECK(r.ReadVarint(v));
        CHECK(v == UINT64_MAX);
    }
    // 第 10 字节有高位 → 拒绝（值超过 64 位）
    {
        std::vector<std::byte> b(10, std::byte{0xFF});
        b[9] = std::byte{0x03};
        Reader r{b};
        std::uint64_t v = 0;
        CHECK(!r.ReadVarint(v));
    }
    // 11 字节全是续行 → 拒绝
    {
        std::vector<std::byte> b(11, std::byte{0xFF});
        Reader r{b};
        std::uint64_t v = 0;
        CHECK(!r.ReadVarint(v));
    }
    // 截断：只有续行字节
    {
        std::byte only{0x80};
        Reader r{std::span<const std::byte>{&only, 1}};
        std::uint64_t v = 0;
        CHECK(!r.ReadVarint(v));
    }
    // 空输入
    {
        Reader r{};
        std::uint64_t v = 0;
        CHECK(!r.ReadVarint(v));
        CHECK(r.Empty());
    }
}

void TestFixed()
{
    std::byte buf[8]{};
    for (unsigned i = 0; i < 8; ++i)
        buf[i] = static_cast<std::byte>(0x11u * (i + 1));
    Reader r32{std::span<const std::byte>{buf, 4}};
    std::uint32_t v32 = 0;
    CHECK(r32.ReadFixed32(v32));
    CHECK(v32 == 0x44332211u);
    CHECK(!r32.ReadFixed32(v32)); // 已经读空
    std::uint64_t too_short = 0;
    CHECK(!r32.ReadFixed64(too_short)); // 剩余不足 8 字节

    Reader r64{std::span<const std::byte>{buf, 8}};
    std::uint64_t v64 = 0;
    CHECK(r64.ReadFixed64(v64));
    CHECK(v64 == 0x8877665544332211ull);
    CHECK(r64.Empty());
}

void TestLengthDelimited()
{
    // 0x03 "abc"
    std::byte buf[4] = {std::byte{0x03}, std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
    Reader r{std::span<const std::byte>{buf, 4}};
    std::span<const std::byte> out;
    CHECK(r.ReadLengthDelimited(out));
    CHECK(out.size() == 3);
    CHECK(std::string(reinterpret_cast<const char*>(out.data()), out.size()) == "abc");
    CHECK(r.Empty());

    // 长度超过剩余字节 → 失败，且游标不前进（只前进过长度前缀）
    std::byte bad[2] = {std::byte{0x05}, std::byte{'a'}};
    Reader r2{std::span<const std::byte>{bad, 2}};
    std::span<const std::byte> out2;
    CHECK(!r2.ReadLengthDelimited(out2));
    CHECK(r2.Offset() == 1);

    // 长度用 varint 表示且恰好等于剩余 → 成功
    std::byte exact[130];
    exact[0] = static_cast<std::byte>(0x80); // 128 变长编码：0x80 0x01
    exact[1] = std::byte{0x01};
    Reader r3{std::span<const std::byte>{exact, 130}};
    std::span<const std::byte> out3;
    CHECK(r3.ReadLengthDelimited(out3));
    CHECK(out3.size() == 128);
}

void TestSkip()
{
    std::byte buf[16] = {std::byte{0x96}, std::byte{0x01}}; // varint 150
    Reader r{std::span<const std::byte>{buf, 2}};
    CHECK(r.Skip(WireType::kVarint));
    CHECK(r.Empty());

    Reader r2{std::span<const std::byte>{buf, 8}};
    CHECK(r2.Skip(WireType::kFixed64));
    CHECK(r2.Empty());

    Reader r3{std::span<const std::byte>{buf, 2}};
    CHECK(!r3.Skip(WireType::kStartGroup)); // group 不支持
    CHECK(!r3.Skip(WireType::kEndGroup));
}

void TestWriter()
{
    Writer w;
    const std::size_t at = w.ReserveLength();
    w.WriteRaw(std::span<const std::byte>{reinterpret_cast<const std::byte*>("ab"), 2});
    w.BackfillLength(at, 2);
    CHECK(w.Size() == 3);
    CHECK(std::to_integer<std::uint8_t>(w.Bytes()[0]) == 2);

    // 127 字节负载：长度仍装进 1 字节
    {
        Writer w2;
        const std::size_t at2 = w2.ReserveLength();
        std::vector<std::byte> payload(127, std::byte{0x5A});
        w2.WriteRaw(payload);
        w2.BackfillLength(at2, payload.size());
        CHECK(w2.Size() == 128);
        CHECK(std::to_integer<std::uint8_t>(w2.Bytes()[0]) == 127);
        CHECK(std::to_integer<std::uint8_t>(w2.Bytes()[127]) == 0x5A);
    }
    // 128 字节负载：长度要 2 字节，触发一次 memmove，负载不能被打乱
    {
        Writer w3;
        const std::size_t at3 = w3.ReserveLength();
        std::vector<std::byte> payload(128);
        for (std::size_t i = 0; i < payload.size(); ++i)
            payload[i] = static_cast<std::byte>(i & 0xFFu);
        w3.WriteRaw(payload);
        w3.BackfillLength(at3, payload.size());
        CHECK(w3.Size() == 130);
        Reader r{w3.Bytes()};
        std::span<const std::byte> out;
        CHECK(r.ReadLengthDelimited(out));
        CHECK(out.size() == 128);
        for (std::size_t i = 0; i < out.size(); ++i)
            CHECK(out[i] == payload[i]);
        CHECK(r.Empty());
    }
    // 扩容：多次写超过初始容量，内容保持顺序
    {
        Writer w4(4);
        for (int i = 0; i < 100; ++i)
            w4.WriteVarint(static_cast<std::uint64_t>(i));
        Reader r{w4.Bytes()};
        for (int i = 0; i < 100; ++i)
        {
            std::uint64_t v = 0;
            CHECK(r.ReadVarint(v));
            CHECK(v == static_cast<std::uint64_t>(i));
        }
        CHECK(r.Empty());
    }
}

void TestReaderPosition()
{
    std::byte buf[3] = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
    Reader r{std::span<const std::byte>{buf, 3}};
    std::uint64_t v = 0;
    CHECK(r.Offset() == 0);
    CHECK(r.ReadVarint(v));
    CHECK(r.Offset() == 1);
    CHECK(r.Remaining() == 2);
    CHECK(r.Rest().size() == 2);
}

} // namespace

int main()
{
    TestVarintBoundaries();
    TestVarintHostile();
    TestFixed();
    TestLengthDelimited();
    TestSkip();
    TestWriter();
    TestReaderPosition();
    std::printf("lua_pb_wire_test: ok\n");
    return 0;
}
