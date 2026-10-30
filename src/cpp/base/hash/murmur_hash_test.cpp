// MurmurHash64A：与独立的 Python 参考实现逐位一致（含 smhasher 的验证值 0x1F0D3804）、不依赖对齐、各种长度余数、
// 与"逐字节拼装"的朴素实现随机对照。无测试框架依赖：失败即 abort。

#include "murmur_hash.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

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

// 接口防呆：整数不能隐式变成种子；旧的 (指针, 长度) 写法不能悄悄把长度当种子
template <typename... A>
concept CanHash = requires(A... a) { MurmurHash64A(a...); };
static_assert(CanHash<std::string_view> && CanHash<const char (&)[6]> && CanHash<std::string>);
static_assert(CanHash<std::string_view, MurmurSeed> && CanHash<const char (&)[6], MurmurSeed>);
static_assert(!CanHash<const char (&)[6], int>);          // MurmurHash64A("hello", 42)
static_assert(!CanHash<std::string_view, std::uint64_t>); // 裸整数种子
static_assert(!CanHash<const char*, std::size_t>);        // 旧写法 MurmurHash64A(ptr, len)
static_assert(!std::is_convertible_v<std::uint64_t, MurmurSeed>);

namespace
{

// 朴素参考：按字节拼出每个 8 字节块（显式小端），与实现里的 memcpy + 条件换序对照。
std::uint64_t NaiveMurmur64A(const unsigned char* data, std::size_t len, std::uint64_t seed)
{
    const std::uint64_t m = 0xc6a4a7935bd1e995ULL;
    std::uint64_t h = seed ^ (len * m);
    const std::size_t blocks = len / 8;
    for (std::size_t b = 0; b < blocks; ++b)
    {
        std::uint64_t k = 0;
        for (int i = 7; i >= 0; --i)
            k = (k << 8) | data[b * 8 + static_cast<std::size_t>(i)];
        k *= m;
        k ^= k >> 47;
        k *= m;
        h ^= k;
        h *= m;
    }
    const std::size_t rem = len & 7;
    if (rem != 0)
    {
        for (std::size_t i = rem; i-- > 0;)
            h ^= static_cast<std::uint64_t>(data[blocks * 8 + i]) << (8 * i);
        h *= m;
    }
    h ^= h >> 47;
    h *= m;
    h ^= h >> 47;
    return h;
}

// smhasher 的 VerificationTest：对长度 0..255 的键（内容 0,1,2,…）用种子 256-i 求哈希，把所有结果拼起来再哈希一次，
// 取前 4 字节（小端）。MurmurHash64A 的公布值是 0x1F0D3804。
void TestSmhasherVerification()
{
    unsigned char key[256];
    std::vector<unsigned char> hashes;
    for (int i = 0; i < 256; ++i)
    {
        key[i] = static_cast<unsigned char>(i);
        const std::uint64_t h =
            MurmurHash64ABytes(key, static_cast<std::size_t>(i), MurmurSeed{static_cast<std::uint64_t>(256 - i)});
        for (int b = 0; b < 8; ++b)
            hashes.push_back(static_cast<unsigned char>(h >> (8 * b)));
    }
    const std::uint64_t final_hash = MurmurHash64ABytes(hashes.data(), hashes.size());
    const std::uint32_t verification = static_cast<std::uint32_t>(final_hash & 0xffffffffu);
    CHECK(verification == 0x1F0D3804u);
}

// 其它语言对拍用的固定向量（由独立的 Python 实现算出）。
void TestKnownVectors()
{
    CHECK(MurmurHash64A("") == 0x0ULL);
    CHECK(MurmurHash64A("a") == 0x071717d2d36b6b11ULL);
    CHECK(MurmurHash64A("hello") == 0x1e68d17c457bf117ULL);
    CHECK(MurmurHash64A("hello world") == 0xd3ba2368a832afceULL);
    CHECK(MurmurHash64A("The quick brown fox jumps over the lazy dog") == 0x5589ca33042a861bULL);
    CHECK(MurmurHash64A("hello", MurmurSeed{42}) == 0xd417125ccb971887ULL);
    unsigned char seq[16];
    for (int i = 0; i < 16; ++i)
        seq[i] = static_cast<unsigned char>(i);
    CHECK(MurmurHash64ABytes(seq, sizeof(seq), MurmurSeed{7}) == 0xd9f01e5bf094dbbfULL);
}

void TestOverloadsAgree()
{
    const std::string s = "player_12345";
    const std::uint64_t a = MurmurHash64ABytes(s.data(), s.size());
    CHECK(MurmurHash64A(s) == a);
    CHECK(MurmurHash64A(std::string_view(s)) == a);
    CHECK(MurmurHash64ABytes(s.c_str(), s.size(), MurmurSeed{0}) == a);
    CHECK(MurmurHash64A(s, MurmurSeed{1}) != a); // 种子起作用
    CHECK(MurmurHash64A(s, MurmurSeed{}) == a);  // 默认种子是 0
    CHECK(MurmurHash64A(std::string_view("abc")) != MurmurHash64A(std::string_view("abd")));
    // 嵌入的 NUL 参与哈希
    const std::string with_nul("a\0b", 3);
    CHECK(MurmurHash64A(with_nul) != MurmurHash64A(std::string("a")));
    CHECK(MurmurHash64A(with_nul) == MurmurHash64ABytes(with_nul.data(), 3));
}

// 不要求对齐：同一份内容放在缓冲区的各个偏移上（0..7），哈希相同（ubsan 下未对齐读会报错）。
void TestUnalignedInput()
{
    std::mt19937_64 rng(7);
    for (int round = 0; round < 200; ++round)
    {
        const std::size_t len = rng() % 100;
        std::vector<unsigned char> content(len);
        for (auto& c : content)
            c = static_cast<unsigned char>(rng());
        const std::uint64_t expect = NaiveMurmur64A(content.data(), len, 5);
        for (std::size_t offset = 0; offset < 8; ++offset)
        {
            std::vector<unsigned char> buf(offset + len + 8);
            std::copy(content.begin(), content.end(), buf.begin() + static_cast<std::ptrdiff_t>(offset));
            CHECK(MurmurHash64ABytes(buf.data() + offset, len, MurmurSeed{5}) == expect);
        }
    }
}

// 各种长度（尤其是 8 的各个余数、长输入）与朴素实现随机对照
void TestRandomAgainstNaive()
{
    std::mt19937_64 rng(11);
    for (int i = 0; i < 20000; ++i)
    {
        const std::size_t len = i < 1000 ? static_cast<std::size_t>(i % 40) : rng() % 3000;
        std::vector<unsigned char> data(len);
        for (auto& c : data)
            c = static_cast<unsigned char>(rng());
        const std::uint64_t seed = (i % 3 == 0) ? 0 : rng();
        CHECK(MurmurHash64ABytes(data.data(), len, MurmurSeed{seed}) == NaiveMurmur64A(data.data(), len, seed));
    }
    // 空输入允许 nullptr
    CHECK(MurmurHash64ABytes(nullptr, 0, MurmurSeed{9}) == NaiveMurmur64A(nullptr, 0, 9));
}

// 分布的粗检：连续 id 取模后各桶大致均匀（稳定哈希用于分片时最基本的要求）
void TestDistributionForSequentialKeys()
{
    constexpr int kBuckets = 16;
    constexpr int kKeys = 160000;
    std::vector<int> count(kBuckets, 0);
    for (std::uint64_t id = 1; id <= kKeys; ++id)
        ++count[MurmurHash64ABytes(&id, sizeof(id)) % kBuckets];
    for (int c : count)
        CHECK(c > kKeys / kBuckets * 95 / 100 && c < kKeys / kBuckets * 105 / 100); // 每个桶偏差 < 5%
}

} // namespace

int main()
{
    TestSmhasherVerification();
    TestKnownVectors();
    TestOverloadsAgree();
    TestUnalignedInput();
    TestRandomAgainstNaive();
    TestDistributionForSequentialKeys();
    std::puts("murmur_hash_test: ok");
    return 0;
}
