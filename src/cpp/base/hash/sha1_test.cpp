// SHA-1：RFC 3174 / NIST 的标准向量、分块喂入与一次性结果一致、跨块边界的长度（55 / 56 / 63 / 64 / 65 字节的填充分界）、
// 摘要之后可复用。无测试框架依赖：失败即 abort。

#include "base/hash/sha1.h"

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

using breeze::Sha1;

std::string Hex(const Sha1::Digest& d)
{
    static const char* kHex = "0123456789abcdef";
    std::string s;
    for (const std::uint8_t b : d)
    {
        s.push_back(kHex[b >> 4]);
        s.push_back(kHex[b & 15]);
    }
    return s;
}

void TestKnownVectors()
{
    CHECK(Hex(Sha1::Hash(std::string_view(""))) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK(Hex(Sha1::Hash(std::string_view("abc"))) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(Hex(Sha1::Hash(std::string_view("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))) ==
          "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    // RFC 3174 的第四条："01234567" 重复 80 次（640 字节）
    std::string rfc;
    for (int i = 0; i < 80; ++i)
        rfc += "01234567";
    CHECK(Hex(Sha1::Hash(std::string_view(rfc))) == "dea356a2cddd90c7a7ecedc5ebb563934f460452");
    // NIST：一百万个 'a'
    Sha1 s;
    const std::string chunk(1000, 'a');
    for (int i = 0; i < 1000; ++i)
        s.Update(std::string_view(chunk));
    CHECK(Hex(s.Final()) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
}

// RFC 6455 §1.3 的例子：key + GUID 的 SHA-1，再 base64（base64 在 base64_test 里测）。这里只验 SHA-1 的十六进制。
void TestWebSocketGuidExample()
{
    const std::string in = "dGhlIHNhbXBsZSBub25jZQ==258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    CHECK(Hex(Sha1::Hash(std::string_view(in))) == "b37a4f2cc0624f1690f64606cf385945b2bec4ea");
}

// 任意切分喂入的结果与一次性相同；覆盖填充的各个分界长度。
void TestChunkedMatchesOneShot()
{
    for (std::size_t len : {0u, 1u, 54u, 55u, 56u, 57u, 63u, 64u, 65u, 119u, 120u, 127u, 128u, 129u, 1000u})
    {
        std::string data(len, '\0');
        for (std::size_t i = 0; i < len; ++i)
            data[i] = static_cast<char>(i * 7 + 3);
        const auto whole = Sha1::Hash(std::string_view(data));
        for (std::size_t step : {1u, 3u, 7u, 63u, 64u, 65u})
        {
            Sha1 s;
            for (std::size_t off = 0; off < len; off += step)
                s.Update(std::string_view(data).substr(off, step));
            CHECK(s.Final() == whole);
        }
    }
}

void TestReusableAfterFinal()
{
    Sha1 s;
    s.Update(std::string_view("abc"));
    CHECK(Hex(s.Final()) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    s.Update(std::string_view("abc"));
    CHECK(Hex(s.Final()) == "a9993e364706816aba3e25717850c26c9cd0d89d");
}

} // namespace

int main()
{
    TestKnownVectors();
    TestWebSocketGuidExample();
    TestChunkedMatchesOneShot();
    TestReusableAfterFinal();
    std::puts("sha1_test: ok");
    return 0;
}
