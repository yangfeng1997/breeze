// 无测试框架依赖：失败即 abort。

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "buffer.h"
#include "length_prefix_codec.h"

using namespace breeze;

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

using Bytes = std::vector<std::byte>;

std::span<const std::byte> View(const Bytes& v) { return {v.data(), v.size()}; }

// 用 EncodeHeader 拼出一帧：帧头 + payload。
Bytes Frame(LengthPrefixCodec& codec, const std::string& payload)
{
    std::array<std::byte, kMaxFrameHeaderSize> hdr{};
    const std::size_t n = codec.EncodeHeader(payload.size(), hdr);
    CHECK(n == LengthPrefixCodec::kHeaderSize);
    Bytes out(hdr.begin(), hdr.begin() + static_cast<std::ptrdiff_t>(n));
    for (const char c : payload)
        out.push_back(static_cast<std::byte>(c));
    return out;
}

// 把缓冲里能解的帧都解出来；返回最后一次 Decode 的结果（NeedMore 或 Error）。
DecodeResult Drain(FrameCodec& codec, Buffer& buf, std::vector<std::string>& frames)
{
    for (;;)
    {
        std::size_t used = 0;
        const DecodeResult r = codec.Decode(
            buf.Readable(),
            [&](std::span<const std::byte> p)
            { frames.emplace_back(reinterpret_cast<const char*>(p.data()), p.size()); },
            used);
        if (r != DecodeResult::Ok)
        {
            CHECK(used == 0); // NeedMore / Error 都不推进
            return r;
        }
        buf.Consume(used);
    }
}

const std::vector<std::string> kPayloads = {"a", "", "hello world", std::string(300, 'z'), "tail"};

Bytes Stream(LengthPrefixCodec& codec)
{
    Bytes s;
    for (const auto& p : kPayloads)
    {
        const Bytes f = Frame(codec, p);
        s.insert(s.end(), f.begin(), f.end());
    }
    return s;
}

void TestWholeFeed()
{
    LengthPrefixCodec codec;
    Buffer buf;
    buf.Append(View(Stream(codec)));
    std::vector<std::string> got;
    CHECK(Drain(codec, buf, got) == DecodeResult::NeedMore);
    CHECK(got == kPayloads);
    CHECK(buf.ReadableBytes() == 0);
}

// 逐字节喂入的结果必须与整块喂入一致；NeedMore 不得消耗任何字节。
void TestByteByByte()
{
    LengthPrefixCodec enc;
    const Bytes stream = Stream(enc);

    LengthPrefixCodec codec;
    Buffer buf;
    std::vector<std::string> got;
    std::size_t consumed = 0;
    for (std::size_t i = 0; i < stream.size(); ++i)
    {
        buf.Append(std::span<const std::byte>(&stream[i], 1));
        const std::size_t before = got.size();
        CHECK(Drain(codec, buf, got) == DecodeResult::NeedMore);
        for (std::size_t k = before; k < got.size(); ++k)
            consumed += LengthPrefixCodec::kHeaderSize + got[k].size();
        CHECK(buf.ReadableBytes() == i + 1 - consumed); // 没解出整帧的字节原样留着
    }
    CHECK(got == kPayloads);
}

// 在流的每一个位置切成两段喂入。
void TestEverySplitPoint()
{
    LengthPrefixCodec enc;
    const Bytes stream = Stream(enc);
    for (std::size_t k = 0; k <= stream.size(); ++k)
    {
        LengthPrefixCodec codec;
        Buffer buf;
        std::vector<std::string> got;
        buf.Append(View(Bytes(stream.begin(), stream.begin() + static_cast<std::ptrdiff_t>(k))));
        CHECK(Drain(codec, buf, got) == DecodeResult::NeedMore);
        buf.Append(View(Bytes(stream.begin() + static_cast<std::ptrdiff_t>(k), stream.end())));
        CHECK(Drain(codec, buf, got) == DecodeResult::NeedMore);
        CHECK(got == kPayloads);
    }
}

void TestPartialHeaderNeedsMore()
{
    LengthPrefixCodec codec;
    Buffer buf;
    const Bytes three = {std::byte{0}, std::byte{0}, std::byte{0}};
    buf.Append(View(three));
    std::vector<std::string> got;
    CHECK(Drain(codec, buf, got) == DecodeResult::NeedMore);
    CHECK(got.empty() && buf.ReadableBytes() == 3);
}

void TestZeroLengthFrame()
{
    LengthPrefixCodec codec;
    Buffer buf;
    buf.Append(View(Frame(codec, "")));
    std::vector<std::string> got;
    CHECK(Drain(codec, buf, got) == DecodeResult::NeedMore);
    CHECK(got.size() == 1 && got[0].empty());
}

void TestMaxFrameBoundary()
{
    LengthPrefixCodec codec(16);
    {
        Buffer buf;
        buf.Append(View(Frame(codec, std::string(16, 'a')))); // 恰好等于上限：合法
        std::vector<std::string> got;
        CHECK(Drain(codec, buf, got) == DecodeResult::NeedMore);
        CHECK(got.size() == 1 && got[0].size() == 16);
    }
    {
        // 只发来 4 字节帧头、声明 17 > 上限：立刻判错，不等 payload，缓冲也不会为它增长。
        Buffer buf;
        const Bytes hdr = {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{17}};
        buf.Append(View(hdr));
        std::vector<std::string> got;
        CHECK(Drain(codec, buf, got) == DecodeResult::Error);
        CHECK(got.empty());
        CHECK(buf.ReadableBytes() == 4); // Error 不消耗字节，由调用方关闭连接
    }
    {
        // 恶意的超大长度
        Buffer buf;
        const Bytes hdr = {std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff}};
        buf.Append(View(hdr));
        std::vector<std::string> got;
        CHECK(Drain(codec, buf, got) == DecodeResult::Error);
    }
}

void TestEncodeHeader()
{
    LengthPrefixCodec codec(1000);
    std::array<std::byte, kMaxFrameHeaderSize> hdr{};
    CHECK(codec.EncodeHeader(5, hdr) == 4);
    CHECK(hdr[0] == std::byte{0} && hdr[1] == std::byte{0} && hdr[2] == std::byte{0} && hdr[3] == std::byte{5});
    CHECK(codec.EncodeHeader(0x0102, hdr) == 4); // 大端
    CHECK(hdr[2] == std::byte{1} && hdr[3] == std::byte{2});
    CHECK(codec.EncodeHeader(1000, hdr) == 4);
    CHECK(codec.EncodeHeader(1001, hdr) == 0); // 超过上限
}

// EncodeHeader 与 Decode 往返：各种长度。
void TestRoundTripVariousLengths()
{
    LengthPrefixCodec codec(70000);
    Buffer buf;
    std::vector<std::string> want;
    for (const std::size_t n : {0u, 1u, 2u, 255u, 256u, 257u, 65535u, 65536u, 70000u})
    {
        want.emplace_back(n, static_cast<char>('a' + static_cast<int>(n % 26)));
        buf.Append(View(Frame(codec, want.back())));
    }
    std::vector<std::string> got;
    CHECK(Drain(codec, buf, got) == DecodeResult::NeedMore);
    CHECK(got == want);
}

// sink 看到的是缓冲内部的视图，sink 返回后才消耗这一帧。
void TestSinkSeesViewIntoData()
{
    LengthPrefixCodec codec;
    const Bytes frame = Frame(codec, "abc");
    std::size_t used = 99;
    bool called = false;
    const auto r = codec.Decode(
        View(frame),
        [&](std::span<const std::byte> p)
        {
            called = true;
            CHECK(p.size() == 3);
            CHECK(p.data() == frame.data() + LengthPrefixCodec::kHeaderSize); // 视图就在 data 里，没有拷贝
        },
        used);
    CHECK(r == DecodeResult::Ok && called);
    CHECK(used == frame.size());

    // data 里还有后续字节：只解第一帧，used 只覆盖它
    Bytes two = frame;
    const Bytes next = Frame(codec, "defg");
    two.insert(two.end(), next.begin(), next.end());
    called = false;
    used = 0;
    CHECK(codec.Decode(View(two), [&](std::span<const std::byte>) { called = true; }, used) == DecodeResult::Ok);
    CHECK(called && used == frame.size());

    // 不完整：NeedMore，used 被清零
    used = 99;
    const Bytes partial(frame.begin(), frame.end() - 1);
    CHECK(
        codec.Decode(View(partial), [](std::span<const std::byte>) { CHECK(false); }, used) == DecodeResult::NeedMore);
    CHECK(used == 0);
}

void TestFactoryCreatesIndependentInstances()
{
    const LengthPrefixCodecFactory factory(32);
    const auto a = factory.Create();
    const auto b = factory.Create();
    CHECK(a && b && a.get() != b.get());

    // 一个实例的半包状态不影响另一个（各自配各自的缓冲）
    LengthPrefixCodec enc(32);
    Buffer ba;
    Buffer bb;
    const Bytes f = Frame(enc, "xy");
    ba.Append(View(Bytes(f.begin(), f.begin() + 3)));
    bb.Append(View(f));
    std::vector<std::string> ga;
    std::vector<std::string> gb;
    CHECK(Drain(*a, ba, ga) == DecodeResult::NeedMore && ga.empty());
    CHECK(Drain(*b, bb, gb) == DecodeResult::NeedMore && gb.size() == 1);
    // 上限由工厂参数决定
    std::array<std::byte, kMaxFrameHeaderSize> hdr{};
    CHECK(a->EncodeHeader(32, hdr) == 4);
    CHECK(a->EncodeHeader(33, hdr) == 0);
}

} // namespace

int main()
{
    TestWholeFeed();
    TestByteByByte();
    TestEverySplitPoint();
    TestPartialHeaderNeedsMore();
    TestZeroLengthFrame();
    TestMaxFrameBoundary();
    TestEncodeHeader();
    TestRoundTripVariousLengths();
    TestSinkSeesViewIntoData();
    TestFactoryCreatesIndependentInstances();
    std::puts("codec_test: ok");
    return 0;
}
