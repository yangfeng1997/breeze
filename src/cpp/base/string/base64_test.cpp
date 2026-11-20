// base64：RFC 4648 §10 的标准向量、往返、严格解码（长度 / 字符 / 填充位置 / 非规范填充位 / 空白）、
// WebSocket 握手的 RFC 6455 §1.3 例子（Sec-WebSocket-Key → Sec-WebSocket-Accept，连同 SHA-1）。
// 无测试框架依赖：失败即 abort。

#include "base/string/base64.h"

#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "base/hash/sha1.h"

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
using breeze::str::Base64Decode;
using breeze::str::Base64Encode;

std::string Str(const std::vector<std::byte>& v) { return {reinterpret_cast<const char*>(v.data()), v.size()}; }

// RFC 4648 §10
void TestStandardVectors()
{
    const std::pair<const char*, const char*> kCases[] = {
        {"", ""}, {"f", "Zg=="}, {"fo", "Zm8="}, {"foo", "Zm9v"}, {"foob", "Zm9vYg=="}, {"fooba", "Zm9vYmE="}, {"foobar", "Zm9vYmFy"}};
    for (const auto& [plain, enc] : kCases)
    {
        CHECK(Base64Encode(std::string_view(plain)) == enc);
        const auto dec = Base64Decode(enc);
        CHECK(dec && Str(*dec) == plain);
    }
}

void TestRoundTripAllByteValues()
{
    for (std::size_t len = 0; len < 40; ++len)
    {
        std::string s(len, '\0');
        for (std::size_t i = 0; i < len; ++i)
            s[i] = static_cast<char>(i * 37 + len);
        const auto dec = Base64Decode(Base64Encode(std::string_view(s)));
        CHECK(dec && Str(*dec) == s);
    }
    std::string all;
    for (int i = 0; i < 256; ++i)
        all.push_back(static_cast<char>(i));
    const auto dec = Base64Decode(Base64Encode(std::string_view(all)));
    CHECK(dec && Str(*dec) == all);
}

// 握手头来自不可信的对端：严格拒绝一切非规范写法。
void TestStrictDecodeRejects()
{
    CHECK(!Base64Decode("Zg="));       // 长度不是 4 的倍数
    CHECK(!Base64Decode("Zg"));        // 缺填充
    CHECK(!Base64Decode("Zm9v\n"));    // 空白
    CHECK(!Base64Decode("Zm9v Zg=="));
    CHECK(!Base64Decode("Zm9-"));      // URL 安全字母表
    CHECK(!Base64Decode("Zm9_"));
    CHECK(!Base64Decode("Zg=A"));      // '=' 后面不能再有数据字符
    CHECK(!Base64Decode("=Zg="));      // '=' 只能在末尾
    CHECK(!Base64Decode("Zg==Zm9v"));  // 填充只能出现在最后一组
    CHECK(!Base64Decode("Zh=="));      // 非规范：'Zh==' 与 'Zg==' 会解出同一个字节
    CHECK(!Base64Decode("Zm9="));      // 非规范：低 2 位不为 0
    CHECK(Base64Decode("Zm8=").has_value());
}

// RFC 6455 §1.3：key = "dGhlIHNhbXBsZSBub25jZQ=="，Accept = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="
void TestWebSocketAcceptExample()
{
    const std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
    const auto digest = Sha1::Hash(std::string_view(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
    CHECK(Base64Encode(std::as_bytes(std::span(digest))) == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    // key 本身必须是 16 字节的 base64
    const auto raw = Base64Decode(key);
    CHECK(raw && raw->size() == 16);
}

} // namespace

int main()
{
    TestStandardVectors();
    TestRoundTripAllByteValues();
    TestStrictDecodeRejects();
    TestWebSocketAcceptExample();
    std::puts("base64_test: ok");
    return 0;
}
