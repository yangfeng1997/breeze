// 信封：布局、解析校验、过期判定、转发时的改写。
// 无测试框架依赖：失败即 abort。

#include "base/net/agent/envelope.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                              \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (0)

using namespace breeze::agent;

namespace
{

EnvelopeHead ValidHead(std::uint32_t body_len = 10)
{
    EnvelopeHead h{};
    h.magic = kEnvelopeMagic;
    h.version = kEnvelopeVersion;
    h.type = static_cast<std::uint8_t>(EnvType::kRequest);
    h.body_len = body_len;
    h.msg_id = 7;
    h.src = 0x00010101;
    h.dst = 0x00010202;
    h.service_type = 5;
    h.route_type = static_cast<std::uint8_t>(RouteType::kDirect);
    h.route_key = 99;
    h.remaining_ms = 3000;
    h.seq = 1234567890123ULL;
    h.trace_id = 42;
    return h;
}

std::vector<std::byte> Wire(const EnvelopeHead& h, std::size_t total)
{
    std::vector<std::byte> v(total);
    std::memcpy(v.data(), &h, sizeof(h) < total ? sizeof(h) : total);
    return v;
}

EnvStatus Parse(const EnvelopeHead& h, std::size_t total)
{
    EnvelopeHead out{};
    return ParseEnvelope(Wire(h, total), out);
}

void TestLayout()
{
    CHECK(sizeof(EnvelopeHead) == 64);
    // 字段偏移是线格式的一部分：改了就要升 kEnvelopeVersion。
    CHECK(offsetof(EnvelopeHead, magic) == 0 && offsetof(EnvelopeHead, version) == 4 && offsetof(EnvelopeHead, type) == 5);
    CHECK(offsetof(EnvelopeHead, flags) == 6 && offsetof(EnvelopeHead, body_len) == 8 && offsetof(EnvelopeHead, msg_id) == 12);
    CHECK(offsetof(EnvelopeHead, src) == 16 && offsetof(EnvelopeHead, dst) == 20 && offsetof(EnvelopeHead, service_type) == 24);
    CHECK(offsetof(EnvelopeHead, route_type) == 26 && offsetof(EnvelopeHead, hops) == 27 && offsetof(EnvelopeHead, gray_tag) == 28);
    CHECK(offsetof(EnvelopeHead, reserved0) == 29 && offsetof(EnvelopeHead, route_key) == 32);
    CHECK(offsetof(EnvelopeHead, remaining_ms) == 40 && offsetof(EnvelopeHead, err_code) == 44);
    CHECK(offsetof(EnvelopeHead, seq) == 48 && offsetof(EnvelopeHead, trace_id) == 56);
    // 魔数的字节序列是 'B' 'E' 'N' 'V'，抓包 / hexdump 时一眼能认。
    const EnvelopeHead h = ValidHead();
    CHECK(std::memcmp(&h.magic, "BENV", 4) == 0);
}

void TestParseOk()
{
    for (const auto type : {EnvType::kRequest, EnvType::kOneway})
    {
        for (const auto route : {RouteType::kDirect, RouteType::kAny, RouteType::kHash, RouteType::kBroadcast})
        {
            EnvelopeHead h = ValidHead();
            h.type = static_cast<std::uint8_t>(type);
            h.route_type = static_cast<std::uint8_t>(route);
            if (route != RouteType::kDirect)
                h.dst = 0; // 非 Direct 不需要 dst
            EnvelopeHead out{};
            CHECK(ParseEnvelope(Wire(h, 64 + 10), out) == EnvStatus::kOk);
            CHECK(std::memcmp(&out, &h, sizeof(h)) == 0);
        }
    }
    EnvelopeHead resp = ValidHead(0);
    resp.type = static_cast<std::uint8_t>(EnvType::kResponse);
    resp.err_code = 5;
    CHECK(Parse(resp, 64) == EnvStatus::kOk); // 没有载荷也合法
    EnvelopeHead traced = ValidHead();
    traced.flags = kEnvFlagTrace;
    CHECK(Parse(traced, 74) == EnvStatus::kOk);
}

void TestParseRejects()
{
    EnvelopeHead ok = ValidHead();
    CHECK(Parse(ok, 74) == EnvStatus::kOk);

    // 长度：不足 64 一律 TooShort（包括 0）；body_len 与总长差一个字节都不行；body_len 取极值也不能让相减回绕。
    EnvelopeHead out{};
    CHECK(ParseEnvelope({}, out) == EnvStatus::kTooShort);
    CHECK(Parse(ok, 63) == EnvStatus::kTooShort);
    CHECK(Parse(ok, 64) == EnvStatus::kBadLength);
    CHECK(Parse(ok, 73) == EnvStatus::kBadLength);
    CHECK(Parse(ok, 75) == EnvStatus::kBadLength);
    EnvelopeHead huge = ValidHead(0xFFFFFFFFu);
    CHECK(Parse(huge, 74) == EnvStatus::kBadLength);
    EnvelopeHead zero = ValidHead(0);
    CHECK(Parse(zero, 64) == EnvStatus::kOk);
    CHECK(Parse(zero, 65) == EnvStatus::kBadLength);

    auto bad = [&](auto mutate, EnvStatus want)
    {
        EnvelopeHead h = ValidHead();
        mutate(h);
        CHECK(Parse(h, 74) == want);
    };
    bad([](EnvelopeHead& h) { h.magic ^= 1; }, EnvStatus::kBadMagic);
    bad([](EnvelopeHead& h) { h.version = 0; }, EnvStatus::kBadVersion);
    bad([](EnvelopeHead& h) { h.version = kEnvelopeVersion + 1; }, EnvStatus::kBadVersion);
    bad([](EnvelopeHead& h) { h.type = 0; }, EnvStatus::kBadType);
    bad([](EnvelopeHead& h) { h.type = 4; }, EnvStatus::kBadType);
    bad([](EnvelopeHead& h) { h.type = 255; }, EnvStatus::kBadType);
    bad([](EnvelopeHead& h) { h.route_type = 0; }, EnvStatus::kBadRoute);
    bad([](EnvelopeHead& h) { h.route_type = 5; }, EnvStatus::kBadRoute);
    bad([](EnvelopeHead& h) { h.dst = 0; }, EnvStatus::kBadRoute); // Direct 必须有 dst
    bad(
        [](EnvelopeHead& h)
        {
            h.type = static_cast<std::uint8_t>(EnvType::kResponse);
            h.route_type = static_cast<std::uint8_t>(RouteType::kAny); // 回包只能点对点
        },
        EnvStatus::kBadRoute);
    bad([](EnvelopeHead& h) { h.flags = 2; }, EnvStatus::kBadFlags);
    bad([](EnvelopeHead& h) { h.flags = 0x8000; }, EnvStatus::kBadFlags);
    for (int i = 0; i < 3; ++i)
        bad([i](EnvelopeHead& h) { h.reserved0[i] = 1; }, EnvStatus::kBadReserved);

    // 先判魔数 / 版本：对端不是我们的协议时，别的字段没有意义。
    bad(
        [](EnvelopeHead& h)
        {
            h.magic = 0;
            h.type = 0;
            h.route_type = 9;
        },
        EnvStatus::kBadMagic);

    // 这些字段不影响合法性（由路由层 / 业务层解释）：随便什么值都过。
    bad([](EnvelopeHead& h) { h.msg_id = 0xFFFFFFFF; }, EnvStatus::kOk);
    bad([](EnvelopeHead& h) { h.src = 0; }, EnvStatus::kOk); // src 由路由层核对身份
    bad([](EnvelopeHead& h) { h.remaining_ms = 0; }, EnvStatus::kOk); // 过期由 CheckExpire 判，单独计数
    bad([](EnvelopeHead& h) { h.hops = 200; }, EnvStatus::kOk);       // 转发次数由 PatchForForward 判
    bad([](EnvelopeHead& h) { h.seq = ~0ULL; }, EnvStatus::kOk);
    bad([](EnvelopeHead& h) { h.gray_tag = 255; }, EnvStatus::kOk);
}

// 解析之后用的是本地拷贝：原缓冲随后怎么改都不影响已经校验过的值。
void TestParseCopiesHeader()
{
    std::vector<std::byte> buf = Wire(ValidHead(), 74);
    EnvelopeHead out{};
    CHECK(ParseEnvelope(buf, out) == EnvStatus::kOk);
    const EnvelopeHead snapshot = out;
    std::memset(buf.data(), 0xFF, buf.size());
    CHECK(std::memcmp(&out, &snapshot, sizeof(out)) == 0);
}

void TestWriteHead()
{
    const EnvelopeHead h = ValidHead();
    std::vector<std::byte> buf(80, std::byte{0xAB});
    WriteEnvelopeHead(buf, h);
    EnvelopeHead back{};
    std::memcpy(&back, buf.data(), sizeof(back));
    CHECK(std::memcmp(&back, &h, sizeof(h)) == 0);
    CHECK(buf[64] == std::byte{0xAB}); // 只写前 64 字节
}

void TestCheckExpire()
{
    constexpr std::uint64_t kMs = 1'000'000;
    constexpr std::uint64_t kT0 = 5'000 * kMs; // 入队时刻（任意非 0 的单调时刻）
    std::uint32_t left = 0;

    // 无效值：remaining_ms 或入队时刻是 0，都按已过期丢弃并单独计数。
    CHECK(CheckExpire(0, kT0, kT0, &left) == ExpireStatus::kInvalid);
    CHECK(CheckExpire(100, 0, kT0, &left) == ExpireStatus::kInvalid);
    CHECK(CheckExpire(0, 0, kT0, &left) == ExpireStatus::kInvalid);

    // 刚入队：剩余不变。
    CHECK(CheckExpire(3000, kT0, kT0, &left) == ExpireStatus::kAlive && left == 3000);
    // 排队 1200ms：扣掉。
    CHECK(CheckExpire(3000, kT0, kT0 + 1200 * kMs, &left) == ExpireStatus::kAlive && left == 1800);
    // 刚好用完 = 过期；差 1 纳秒用完（剩不足 1 毫秒）也算过期：写进去的 remaining_ms 不能是 0。
    CHECK(CheckExpire(3000, kT0, kT0 + 3000 * kMs, &left) == ExpireStatus::kExpired);
    CHECK(CheckExpire(3000, kT0, kT0 + 3000 * kMs - 1, &left) == ExpireStatus::kExpired);
    CHECK(CheckExpire(3000, kT0, kT0 + 2999 * kMs, &left) == ExpireStatus::kAlive && left == 1);
    CHECK(CheckExpire(3000, kT0, kT0 + 2999 * kMs + 1, &left) == ExpireStatus::kExpired); // 零头不到 1 毫秒
    CHECK(CheckExpire(3000, kT0, kT0 + 5000 * kMs, &left) == ExpireStatus::kExpired);
    // 剩余向下取整（宁可早丢）：排队 1.5ms，剩 2998.5ms → 2998。
    CHECK(CheckExpire(3000, kT0, kT0 + 1'500'000, &left) == ExpireStatus::kAlive && left == 2998);
    // 时钟读数比入队时刻还早（不该发生）：当作没排过队。
    CHECK(CheckExpire(3000, kT0, kT0 - 10 * kMs, &left) == ExpireStatus::kAlive && left == 3000);
    // 极值：u32 最大的毫秒数乘 10^6 也不会溢出 u64。
    CHECK(CheckExpire(0xFFFFFFFFu, kT0, kT0 + 1000 * kMs, &left) == ExpireStatus::kAlive && left == 0xFFFFFFFFu - 1000);
    CHECK(CheckExpire(1, kT0, kT0, &left) == ExpireStatus::kAlive && left == 1);
    CHECK(CheckExpire(1, kT0, kT0 + 1, &left) == ExpireStatus::kExpired);
    // left_ms 可以不要。
    CHECK(CheckExpire(3000, kT0, kT0, nullptr) == ExpireStatus::kAlive);
}

void TestPatchForForward()
{
    EnvelopeHead h = ValidHead();
    CHECK(PatchForForward(h, 1800));
    CHECK(h.remaining_ms == 1800 && h.hops == 1);
    // 其余字段原封不动。
    const EnvelopeHead orig = ValidHead();
    EnvelopeHead expect = orig;
    expect.remaining_ms = 1800;
    expect.hops = 1;
    CHECK(std::memcmp(&h, &expect, sizeof(h)) == 0);
    // 到上限：拒绝，头不变。
    EnvelopeHead limit = ValidHead();
    limit.hops = kMaxHops - 1;
    CHECK(PatchForForward(limit, 10) && limit.hops == kMaxHops);
    const EnvelopeHead before = limit;
    CHECK(!PatchForForward(limit, 20));
    CHECK(std::memcmp(&limit, &before, sizeof(limit)) == 0);
}

// 随机翻字节：要么被拒绝，要么通过——通过就必须满足全部不变量。永远不崩、不越界（asan 下跑）。
void TestFuzz()
{
    std::mt19937_64 rng(20261101);
    const EnvelopeHead base = ValidHead(33);
    for (int iter = 0; iter < 200000; ++iter)
    {
        std::vector<std::byte> buf = Wire(base, 64 + 33);
        const int flips = 1 + static_cast<int>(rng() % 4);
        for (int f = 0; f < flips; ++f)
            buf[rng() % 64] ^= static_cast<std::byte>(1u << (rng() % 8));
        if (rng() % 8 == 0)
            buf.resize(rng() % (buf.size() + 1)); // 截断
        EnvelopeHead out{};
        if (ParseEnvelope(buf, out) != EnvStatus::kOk)
            continue;
        CHECK(buf.size() >= 64 && out.magic == kEnvelopeMagic && out.version == kEnvelopeVersion);
        CHECK(out.type >= 1 && out.type <= 3 && out.route_type >= 1 && out.route_type <= 4);
        CHECK((out.flags & ~kEnvKnownFlags) == 0);
        CHECK(out.reserved0[0] == 0 && out.reserved0[1] == 0 && out.reserved0[2] == 0);
        CHECK(static_cast<std::uint64_t>(out.body_len) == buf.size() - 64);
        if (out.route_type == static_cast<std::uint8_t>(RouteType::kDirect))
            CHECK(out.dst != 0);
        if (out.type == static_cast<std::uint8_t>(EnvType::kResponse))
            CHECK(out.route_type == static_cast<std::uint8_t>(RouteType::kDirect));
    }
}

} // namespace

int main()
{
    TestLayout();
    TestParseOk();
    TestParseRejects();
    TestParseCopiesHeader();
    TestWriteHead();
    TestCheckExpire();
    TestPatchForForward();
    TestFuzz();
    std::puts("envelope_test: OK");
    return 0;
}
