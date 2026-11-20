// UTF-8 校验：已知的合法 / 非法序列（RFC 3629 的边界）、与独立的参考实现穷举比对（全部 1~3 字节序列、结构化的 4 字节序列）、
// 任意切分不影响结果（增量校验的性质）、随机数据与参考实现一致。无测试框架依赖：失败即 abort。

#include "base/string/utf8.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

using breeze::str::IsValidUtf8;
using breeze::str::Utf8Validator;

// splitmix64：测试只要确定性的随机序列，不引入 breeze_random（它只在 Linux 网络库开关下才有）。
struct Rng
{
    std::uint64_t s;
    std::uint64_t operator()()
    {
        std::uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    std::uint64_t Below(std::uint64_t n) { return (*this)() % n; }
    bool Chance(unsigned num, unsigned den) { return Below(den) < num; }
};

// 参考实现：按定义逐码点解码、检查范围。与被测代码刻意写成不同的结构（不共用状态机）。
bool RefValid(const std::vector<std::uint8_t>& s)
{
    std::size_t i = 0;
    while (i < s.size())
    {
        const std::uint8_t b = s[i];
        std::size_t len;
        std::uint32_t cp;
        if (b < 0x80)
        {
            len = 1;
            cp = b;
        }
        else if ((b & 0xE0) == 0xC0)
        {
            len = 2;
            cp = b & 0x1F;
        }
        else if ((b & 0xF0) == 0xE0)
        {
            len = 3;
            cp = b & 0x0F;
        }
        else if ((b & 0xF8) == 0xF0)
        {
            len = 4;
            cp = b & 0x07;
        }
        else
        {
            return false;
        }
        if (i + len > s.size())
            return false;
        for (std::size_t k = 1; k < len; ++k)
        {
            if ((s[i + k] & 0xC0) != 0x80)
                return false;
            cp = (cp << 6) | (s[i + k] & 0x3F);
        }
        static constexpr std::uint32_t kMin[5] = {0, 0, 0x80, 0x800, 0x10000};
        if (len > 1 && cp < kMin[len])
            return false; // 过长编码
        if (cp >= 0xD800 && cp <= 0xDFFF)
            return false; // 代理对
        if (cp > 0x10FFFF)
            return false;
        i += len;
    }
    return true;
}

bool Fast(const std::vector<std::uint8_t>& s)
{
    return IsValidUtf8(std::string_view(reinterpret_cast<const char*>(s.data()), s.size()));
}

void TestKnownVectors()
{
    CHECK(IsValidUtf8(""));
    CHECK(IsValidUtf8("hello"));
    CHECK(IsValidUtf8(std::string_view("a\0b", 3))); // NUL 是合法码点
    CHECK(IsValidUtf8("κόσμε"));
    CHECK(IsValidUtf8("\xE4\xBD\xA0\xE5\xA5\xBD"));                 // 你好
    CHECK(IsValidUtf8("\xF0\x9F\x98\x80"));                         // U+1F600
    CHECK(IsValidUtf8("\xC2\x80"));                                 // U+0080，最小的两字节
    CHECK(IsValidUtf8("\xDF\xBF"));                                 // U+07FF
    CHECK(IsValidUtf8("\xE0\xA0\x80"));                             // U+0800
    CHECK(IsValidUtf8("\xED\x9F\xBF"));                             // U+D7FF，代理对之前
    CHECK(IsValidUtf8("\xEE\x80\x80"));                             // U+E000，代理对之后
    CHECK(IsValidUtf8("\xEF\xBF\xBF"));                             // U+FFFF
    CHECK(IsValidUtf8("\xF0\x90\x80\x80"));                         // U+10000
    CHECK(IsValidUtf8("\xF4\x8F\xBF\xBF"));                         // U+10FFFF

    CHECK(!IsValidUtf8("\x80"));                                    // 孤立的后续字节
    CHECK(!IsValidUtf8("\xBF"));
    CHECK(!IsValidUtf8("\xC0\x80"));                                // 过长的 NUL
    CHECK(!IsValidUtf8("\xC1\xBF"));                                // 过长
    CHECK(!IsValidUtf8("\xE0\x80\x80"));                            // 过长
    CHECK(!IsValidUtf8("\xE0\x9F\xBF"));                            // 过长（U+07FF 编成三字节）
    CHECK(!IsValidUtf8("\xF0\x80\x80\x80"));                        // 过长
    CHECK(!IsValidUtf8("\xF0\x8F\xBF\xBF"));                        // 过长（U+FFFF 编成四字节）
    CHECK(!IsValidUtf8("\xED\xA0\x80"));                            // U+D800 代理对
    CHECK(!IsValidUtf8("\xED\xBF\xBF"));                            // U+DFFF
    CHECK(!IsValidUtf8("\xF4\x90\x80\x80"));                        // U+110000
    CHECK(!IsValidUtf8("\xF5\x80\x80\x80"));
    CHECK(!IsValidUtf8("\xF8\x88\x80\x80\x80"));                    // 五字节
    CHECK(!IsValidUtf8("\xFE"));
    CHECK(!IsValidUtf8("\xFF"));
    CHECK(!IsValidUtf8("\xC2"));                                    // 截断
    CHECK(!IsValidUtf8("\xE4\xBD"));
    CHECK(!IsValidUtf8("\xF0\x9F\x98"));
    CHECK(!IsValidUtf8("\xC2\x41"));                                // 后续字节不对
    CHECK(!IsValidUtf8("\xE4\xBD\x41"));
    CHECK(!IsValidUtf8("ok\xC2"));
}

void TestExhaustiveShort()
{
    std::vector<std::uint8_t> s(1);
    for (unsigned a = 0; a < 256; ++a)
    {
        s.assign(1, static_cast<std::uint8_t>(a));
        CHECK(Fast(s) == RefValid(s));
    }
    s.assign(2, 0);
    for (unsigned a = 0; a < 256; ++a)
        for (unsigned b = 0; b < 256; ++b)
        {
            s[0] = static_cast<std::uint8_t>(a);
            s[1] = static_cast<std::uint8_t>(b);
            CHECK(Fast(s) == RefValid(s));
        }
    s.assign(3, 0);
    for (unsigned a = 0; a < 256; ++a)
        for (unsigned b = 0; b < 256; ++b)
            for (unsigned c = 0; c < 256; ++c)
            {
                s[0] = static_cast<std::uint8_t>(a);
                s[1] = static_cast<std::uint8_t>(b);
                s[2] = static_cast<std::uint8_t>(c);
                CHECK(Fast(s) == RefValid(s));
            }
}

// 四字节：首字节全取，后三个字节各取一组边界值（两侧各一个），覆盖所有范围分界。
void TestStructuredFourBytes()
{
    static constexpr std::uint8_t kEdges[] = {0x00, 0x41, 0x7F, 0x80, 0x8F, 0x90, 0x9F, 0xA0, 0xBF, 0xC0, 0xFF};
    std::vector<std::uint8_t> s(4);
    for (unsigned a = 0; a < 256; ++a)
        for (const auto b : kEdges)
            for (const auto c : kEdges)
                for (const auto d : kEdges)
                {
                    s = {static_cast<std::uint8_t>(a), b, c, d};
                    CHECK(Fast(s) == RefValid(s));
                }
}

// 随机数据（偏向非 ASCII，常常恰好是合法序列的变形）：与参考实现一致；任意切分、逐字节喂，结果都与一次喂完相同。
void TestRandomAndChunkInvariance()
{
    Rng rng{20260121};
    for (int iter = 0; iter < 20000; ++iter)
    {
        std::vector<std::uint8_t> s(rng.Below(25));
        const bool mostly_valid = rng.Chance(1, 2);
        for (auto& b : s)
        {
            if (mostly_valid)
            {
                static constexpr std::uint8_t kPool[] = {'a', 'z', 0xC3, 0xA9, 0xE4, 0xBD, 0xA0, 0xF0, 0x9F, 0x98, 0x80, 0x0A};
                b = kPool[rng.Below(sizeof(kPool))];
            }
            else
            {
                b = static_cast<std::uint8_t>(rng());
            }
        }
        const bool expect = RefValid(s);
        CHECK(Fast(s) == expect);

        // 在每个位置切一刀
        for (std::size_t cut = 0; cut <= s.size(); ++cut)
        {
            Utf8Validator v;
            const bool ok1 = v.Feed(std::as_bytes(std::span(s.data(), cut)));
            const bool ok2 = ok1 && v.Feed(std::as_bytes(std::span(s.data() + cut, s.size() - cut)));
            CHECK((ok2 && v.AtBoundary()) == expect);
        }
        // 逐字节
        Utf8Validator v;
        bool ok = true;
        for (std::size_t i = 0; i < s.size() && ok; ++i)
            ok = v.Feed(std::as_bytes(std::span(s.data() + i, 1)));
        CHECK((ok && v.AtBoundary()) == expect);
    }
}

// 长文本：ASCII 快路径（8 字节一组）与多字节字符混排、字符横跨 8 字节边界。
void TestLongMixed()
{
    for (std::size_t prefix = 0; prefix < 20; ++prefix)
    {
        std::string s(prefix, 'x');
        s += "\xE4\xBD\xA0"; // 横跨不同的对齐位置
        s += std::string(37, 'y');
        s += "\xF0\x9F\x98\x80";
        s += std::string(prefix, 'z');
        CHECK(IsValidUtf8(s));
        std::string bad = s;
        bad[prefix + 1] = 'A'; // 把"你"的第二个字节换掉
        CHECK(!IsValidUtf8(bad));
        CHECK(!IsValidUtf8(std::string_view(s).substr(0, s.size() - prefix - 1))); // 切在四字节字符中间
    }
}

} // namespace

int main()
{
    TestKnownVectors();
    TestExhaustiveShort();
    TestStructuredFourBytes();
    TestRandomAndChunkInvariance();
    TestLongMixed();
    std::puts("utf8_test: ok");
    return 0;
}
