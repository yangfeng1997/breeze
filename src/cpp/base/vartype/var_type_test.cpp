// VarType：构造（所有整数宽度、浮点最短往返、被拒绝的类型）、严格转换与整数的"小数写法"、布尔、与数字 / 字符串的比较、
// 与原版行为的差别（精度丢失、垃圾得 0、并发读缓存竞争）、格式化与哈希。无测试框架依赖：失败即 abort。

#include "var_type.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <limits>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
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

namespace
{

enum Plain
{
    kA
};
enum class Scoped
{
    kB
};

// 能构造的与不能构造的
static_assert(std::is_constructible_v<VarType, int> && std::is_constructible_v<VarType, unsigned long long> &&
              std::is_constructible_v<VarType, long long> && std::is_constructible_v<VarType, std::int8_t> &&
              std::is_constructible_v<VarType, float> && std::is_constructible_v<VarType, double> &&
              std::is_constructible_v<VarType, bool> && std::is_constructible_v<VarType, const char*> &&
              std::is_constructible_v<VarType, std::string> && std::is_constructible_v<VarType, std::string_view>);
static_assert(!std::is_constructible_v<VarType, char> && !std::is_constructible_v<VarType, wchar_t> &&
              !std::is_constructible_v<VarType, char16_t>);
static_assert(!std::is_constructible_v<VarType, Plain> && !std::is_constructible_v<VarType, Scoped>);
// 转换是 explicit：不能隐式变成数字
static_assert(!std::is_convertible_v<VarType, int> && !std::is_convertible_v<VarType, double> &&
              !std::is_convertible_v<VarType, bool> && !std::is_convertible_v<VarType, std::string>);
static_assert(std::is_constructible_v<int, VarType> && std::is_constructible_v<double, VarType> &&
              std::is_constructible_v<bool, VarType>);

template <typename T>
void ExpectRoundTrip(T v)
{
    const VarType vt(v);
    const auto back = vt.TryAs<T>();
    CHECK(back.has_value() && *back == v);
    CHECK(vt == v); // 与自己构造时的值比较也相等
}

void TestIntegerConstructionAndRoundTrip()
{
    CHECK(VarType(42).str() == "42" && VarType(-7).str() == "-7" && VarType(0).str() == "0");
    CHECK(VarType(42u).str() == "42" && VarType(42L).str() == "42" && VarType(42LL).str() == "42");
    CHECK(VarType(std::uint64_t{18446744073709551615ULL}).str() == "18446744073709551615");
    CHECK(VarType(std::int64_t{-9223372036854775807LL - 1}).str() == "-9223372036854775808");
    CHECK(VarType(static_cast<std::size_t>(5)).str() == "5");
    CHECK(VarType(std::int8_t{-5}).str() == "-5" && VarType(std::uint8_t{200}).str() == "200");
    CHECK(VarType(std::int16_t{-300}).str() == "-300");

    ExpectRoundTrip<std::int8_t>(-128);
    ExpectRoundTrip<std::int8_t>(127);
    ExpectRoundTrip<std::uint8_t>(255);
    ExpectRoundTrip<std::int16_t>(std::numeric_limits<std::int16_t>::min());
    ExpectRoundTrip<std::uint16_t>(65535);
    ExpectRoundTrip<std::int32_t>(std::numeric_limits<std::int32_t>::min());
    ExpectRoundTrip<std::int32_t>(std::numeric_limits<std::int32_t>::max());
    ExpectRoundTrip<std::uint32_t>(std::numeric_limits<std::uint32_t>::max());
    ExpectRoundTrip<std::int64_t>(std::numeric_limits<std::int64_t>::min());
    ExpectRoundTrip<std::int64_t>(std::numeric_limits<std::int64_t>::max());
    ExpectRoundTrip<std::uint64_t>(std::numeric_limits<std::uint64_t>::max());
    ExpectRoundTrip<long long>(-1234567890123LL);
    ExpectRoundTrip<unsigned long long>(1234567890123ULL);

    std::mt19937_64 rng(3);
    for (int i = 0; i < 20000; ++i)
    {
        ExpectRoundTrip<std::int64_t>(static_cast<std::int64_t>(rng()));
        ExpectRoundTrip<std::uint64_t>(rng());
        ExpectRoundTrip<std::int32_t>(static_cast<std::int32_t>(rng()));
    }
}

// 原版用 std::to_string（6 位小数）存浮点：1e-9 变成 "0.000000"，0.1+0.2 读回来不是原值。现在是最短往返。
void TestFloatingRoundTripIsExact()
{
    CHECK(VarType(1e-9).As<double>() == 1e-9);
    CHECK(VarType(0.1 + 0.2).As<double>() == 0.1 + 0.2);
    CHECK(VarType(1.5f).str() == "1.5" && VarType(0.1).str() == "0.1" && VarType(2.0).str() == "2");
    CHECK(VarType(0.1f).As<float>() == 0.1f);
    CHECK(VarType(std::numeric_limits<double>::max()).As<double>() == std::numeric_limits<double>::max());
    CHECK(VarType(std::numeric_limits<double>::denorm_min()).As<double>() == std::numeric_limits<double>::denorm_min());
    CHECK(VarType(-0.0).As<double>() == 0.0);

    std::mt19937_64 rng(5);
    int checked = 0;
    for (int i = 0; i < 200000; ++i)
    {
        const std::uint64_t bits = rng();
        double d;
        std::memcpy(&d, &bits, sizeof(d));
        if (!std::isfinite(d))
            continue; // inf / nan 转不回来（见 TestNonFinite）
        const auto back = VarType(d).TryAs<double>();
        CHECK(back.has_value() && *back == d);
        ++checked;
        const std::uint32_t fb = static_cast<std::uint32_t>(rng());
        float f;
        std::memcpy(&f, &fb, sizeof(f));
        if (std::isfinite(f))
        {
            const auto fback = VarType(f).TryAs<float>();
            CHECK(fback.has_value() && *fback == f);
        }
    }
    CHECK(checked > 190000);
}

void TestNonFinite()
{
    // inf / nan 能存、但转不回来：配置里出现它们几乎一定是笔误
    CHECK(!VarType(std::numeric_limits<double>::infinity()).TryAs<double>().has_value());
    CHECK(!VarType(std::numeric_limits<double>::quiet_NaN()).TryAs<double>().has_value());
    CHECK(!VarType("inf").TryAs<double>().has_value() && !VarType("-inf").TryAs<double>().has_value() &&
          !VarType("nan").TryAs<double>().has_value() && !VarType("infinity").TryAs<double>().has_value());
    CHECK(!VarType("inf").TryAs<int>().has_value());
}

void TestBoolAndStringConstruction()
{
    CHECK(VarType(true).str() == "true" && VarType(false).str() == "false");
    CHECK(VarType(true).As<bool>() && !VarType(false).As<bool>());

    CHECK(VarType().str().empty() && VarType().empty());
    CHECK(VarType("abc").str() == "abc");
    CHECK(VarType(std::string("def")).str() == "def");
    std::string s = "ghi";
    CHECK(VarType(s).str() == "ghi" && s == "ghi");
    VarType moved(std::move(s));
    CHECK(moved.str() == "ghi");
    CHECK(VarType(std::string_view("jkl")).str() == "jkl");
    const std::string nul("a\0b", 3);
    CHECK(VarType(nul).str().size() == 3);    // std::string 里的 NUL 保留
    CHECK(VarType("a\0b").str().size() == 1); // const char* 在 NUL 处结束
}

void TestNullCString()
{
    const char* null = nullptr;
    CHECK(VarType(null).str().empty()); // 空指针得空串，不崩（与原版一致）
}

void TestStrictConversion()
{
    const auto I = [](const char* s) { return VarType(s).TryAs<int>(); };
    CHECK(I("42") == std::optional<int>(42) && I("-42") == std::optional<int>(-42) &&
          I("+42") == std::optional<int>(42));
    CHECK(I(" 42 ") == std::optional<int>(42) && I("\t42\n") == std::optional<int>(42)); // 两端空白
    CHECK(I("007") == std::optional<int>(7));
    // 原版的 strtoll 读到哪算哪、读不到得 0；现在整串必须是一个数
    for (const char* bad : {"", " ", "abc", "12abc", "1 2", "0x10", "--1", "+-1", "++1", "+", "-", "1,000", "１２"})
        CHECK(!I(bad).has_value());
    // 溢出
    CHECK(!I("2147483648").has_value() && I("2147483647") == std::optional<int>(2147483647));
    CHECK(!I("-2147483649").has_value());
    CHECK(!VarType("300").TryAs<std::uint8_t>().has_value() &&
          VarType("255").TryAs<std::uint8_t>() == std::optional<std::uint8_t>(255));
    CHECK(!VarType("-1").TryAs<std::uint32_t>().has_value() && !VarType("-1").TryAs<std::uint64_t>().has_value());
    CHECK(VarType("18446744073709551615").TryAs<std::uint64_t>().has_value());
    CHECK(!VarType("18446744073709551616").TryAs<std::uint64_t>().has_value());
    CHECK(!VarType("9223372036854775808").TryAs<std::int64_t>().has_value());

    const auto D = [](const char* s) { return VarType(s).TryAs<double>(); };
    CHECK(D("3.5") == std::optional<double>(3.5) && D("-2.25") == std::optional<double>(-2.25) &&
          D("+1e3") == std::optional<double>(1000.0));
    CHECK(D(".5") == std::optional<double>(0.5) && D("5.") == std::optional<double>(5.0) &&
          D(" 1.5 ") == std::optional<double>(1.5));
    CHECK(!D("").has_value() && !D("1.5x").has_value() && !D("1e999").has_value() && !D("1.2.3").has_value() &&
          !D(".").has_value());
    CHECK(VarType("0.1").TryAs<float>() == std::optional<float>(0.1f));
    CHECK(!VarType("1e100").TryAs<float>().has_value()); // float 放不下
}

// 整数类型也接受"恰好是整数的小数写法"（表格软件会把 5 写成 5.0）
void TestIntegerAcceptsWholeDecimalForms()
{
    const auto I = [](const char* s) { return VarType(s).TryAs<std::int64_t>(); };
    CHECK(I("5.0") == std::optional<std::int64_t>(5) && I("-2.00") == std::optional<std::int64_t>(-2));
    CHECK(I("1e3") == std::optional<std::int64_t>(1000) && I("1.5e2") == std::optional<std::int64_t>(150));
    CHECK(I("+7.0") == std::optional<std::int64_t>(7) && I("0.0") == std::optional<std::int64_t>(0) &&
          I("-0.0") == std::optional<std::int64_t>(0));
    CHECK(!I("5.5").has_value() && !I("0.1").has_value() && !I("1e-3").has_value());
    // double 表示不了的大整数：不悄悄丢精度
    CHECK(!I("9007199254740993.0").has_value());
    CHECK(!I("9007199254740992.0").has_value()); // 2^53 本身也不行：它与 2^53+1 在 double 里是同一个数
    CHECK(I("9007199254740991.0") == std::optional<std::int64_t>(9007199254740991LL));
    CHECK(I("9007199254740993") == std::optional<std::int64_t>(9007199254740993LL)); // 纯整数写法精确
    // 范围检查
    CHECK(!VarType("300.0").TryAs<std::uint8_t>().has_value() &&
          VarType("200.0").TryAs<std::uint8_t>() == std::optional<std::uint8_t>(200));
    CHECK(!VarType("-1.0").TryAs<std::uint32_t>().has_value());
    CHECK(VarType("-0.0").TryAs<std::uint32_t>() == std::optional<std::uint32_t>(0));
    CHECK(!VarType("1e20").TryAs<std::int64_t>().has_value());
}

void TestBoolConversion()
{
    const auto B = [](const char* s) { return VarType(s).TryAs<bool>(); };
    for (const char* t : {"true", "TRUE", "True", "yes", "YES", "on", "1", "2", "-1", "0.5", "1.0", "100", " true "})
        CHECK(B(t) == std::optional<bool>(true));
    for (const char* f : {"false", "FALSE", "no", "off", "0", "0.0", "-0", " 0 "})
        CHECK(B(f) == std::optional<bool>(false));
    for (const char* bad : {"", "abc", "tru", "yes!", "nan", "inf"})
        CHECK(!B(bad).has_value());
}

void TestAsAndExplicitConversions()
{
    const VarType v("42"), bad("abc"), f("3.5"), t("true");
    CHECK(v.As<int>() == 42 && bad.As<int>() == 0 && bad.As<int>(-1) == -1);
    CHECK(f.As<double>() == 3.5 && f.As<int>(9) == 9); // 3.5 不是整数
    CHECK(t.As<bool>() && !bad.As<bool>() && bad.As<bool>(true));

    CHECK(static_cast<int>(v) == 42 && static_cast<std::int64_t>(v) == 42 && static_cast<double>(f) == 3.5);
    CHECK(static_cast<float>(f) == 3.5f && static_cast<std::uint8_t>(v) == 42);
    CHECK(static_cast<bool>(t) && !static_cast<bool>(bad)); // 失败得 false / 0
    CHECK(static_cast<int>(bad) == 0);
    if (t)
    {
    }
    else
    {
        CHECK(false);
    }
    CHECK(!bad);
    CHECK(static_cast<const std::string&>(v) == "42");
}

void TestComparisonWithNumbers()
{
    const VarType v("42");
    CHECK(v == 42 && 42 == v && v != 41 && 41 != v && !(v == 41) && !(v != 42));
    CHECK(v == 42u && v == 42L && v == 42LL && v == std::int8_t{42} && v == std::uint64_t{42});
    CHECK(VarType(-5) == -5 && VarType(std::int64_t{-5}) == -5);

    // 转换失败就是不相等（原版：垃圾得 0，VarType("abc") == 0 为 true）
    CHECK(!(VarType("abc") == 0) && VarType("abc") != 0 && !(VarType("") == 0) && !(VarType("12abc") == 12));
    // 越界不按截断比较
    CHECK(!(VarType("300") == std::uint8_t{44}) && !(VarType("-1") == std::uint32_t{4294967295u}));
    CHECK(!(VarType("4294967296") == 0) && VarType("4294967296") == std::int64_t{4294967296LL});
    // 整数写成小数
    CHECK(VarType("5.0") == 5 && VarType("1e3") == 1000);
    // 浮点：几个 ulp 内相等
    CHECK(VarType("0.3") == (0.1 + 0.2) && (0.1 + 0.2) == VarType("0.3"));
    CHECK(VarType("0.1") == 0.1f && VarType("3.5") == 3.5);
    CHECK(!(VarType("0.3") == 0.31) && VarType("0.3") != 0.31 && !(VarType("1e10") == 1.0000001e10));
    CHECK(VarType("1e10") == 1e10 && VarType("-0.0") == 0.0);
    CHECK(!(VarType("abc") == 1.0));
    // 相对误差：不是固定的绝对误差。小量照样分得清（绝对误差 1e-3 会判相等），大量里差几个 ulp 仍算相等
    CHECK(!(VarType("1.0005") == 1.0) && !(VarType("1e-9") == 2e-9));
    CHECK(VarType("1000000000000000") == 1e15 + 0.125 && VarType("1e15") != 1e15 + 8.0);
    // 与整数写法的浮点比较
    CHECK(VarType("5") == 5.0 && VarType("5") != 5.5);

    // 布尔
    CHECK(VarType("true") == true && VarType("1") == true && VarType("yes") == true && true == VarType("on"));
    CHECK(VarType("false") == false && VarType("0") == false && VarType("true") != false);
    CHECK(!(VarType("abc") == true) && !(VarType("abc") == false));
    CHECK(VarType(true) == true && VarType(false) == false);
}

void TestComparisonWithStringsAndVarType()
{
    const VarType v("abc");
    CHECK(v == "abc" && "abc" == v && v != "abd" && "abd" != v);
    CHECK(v == std::string("abc") && std::string("abc") == v && v != std::string("x"));
    CHECK(v == std::string_view("abc") && v != std::string_view("abcd"));
    const char* raw = "abc";
    CHECK(v == raw);
    // VarType 之间：文本完全相同
    CHECK(v == VarType("abc") && v != VarType("ABC") && VarType("1") != VarType("1.0") &&
          VarType("01") != VarType("1"));
    CHECK(VarType(1) == VarType("1") && VarType() == VarType(""));
    // 数字写成的 VarType 与字符串字面量比较是比文本
    CHECK(VarType(42) == "42" && VarType(42) != "42.0");
}

void TestNumericEqual()
{
    CHECK(VarType("1").NumericEqual(VarType("1.0")) && VarType("01").NumericEqual(VarType("1")));
    CHECK(VarType("1e2").NumericEqual(VarType("100")) && VarType("-0").NumericEqual(VarType("0")));
    CHECK(!VarType("1").NumericEqual(VarType("2")) && !VarType("1.5").NumericEqual(VarType("1.6")));
    CHECK(VarType("0.3").NumericEqual(VarType("0.30000000000000004")));
    // 大整数精确比较，不被 double 抹平
    CHECK(!VarType("9007199254740993").NumericEqual(VarType("9007199254740992")));
    CHECK(VarType("9007199254740993").NumericEqual(VarType("9007199254740993")));
    // 不是数字：退回文本比较
    CHECK(VarType("abc").NumericEqual(VarType("abc")) && !VarType("abc").NumericEqual(VarType("abd")));
    CHECK(!VarType("abc").NumericEqual(VarType("1")) && !VarType("").NumericEqual(VarType("0")));
}

void TestCopyMoveAndAssignment()
{
    VarType a = 5;
    VarType b = a;
    CHECK(b == 5 && a == 5);
    b = 7;
    CHECK(b == 7 && a == 5);
    b = "hello";
    CHECK(b == "hello");
    VarType c = std::move(b);
    CHECK(c == "hello");
    b = 3.5; // 被移走的对象可以重新赋值使用 // NOLINT
    CHECK(b == 3.5);
    VarType& alias = a;
    a = alias; // 自赋值
    CHECK(a == 5);
}

// 原版的转换缓存是 mutable 的：多个线程同时读同一个 const VarType 是数据竞争（tsan 会报；非 tsan
// 下也可能读到撕裂的值）。 现在对象不可变，各线程读到的值必须一致。
void TestConcurrentConstReads()
{
    const VarType v("123456789");
    const VarType d("2.5");
    std::atomic<int> bad{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < 8; ++t)
    {
        ts.emplace_back(
            [&]
            {
                for (int i = 0; i < 20000; ++i)
                {
                    if (v.As<std::int64_t>() != 123456789 || v.As<double>() != 123456789.0 || d.As<double>() != 2.5 ||
                        !(v == 123456789) || !(d == 2.5) || v.As<bool>() != true)
                    {
                        ++bad;
                    }
                }
            });
    }
    for (auto& t : ts)
        t.join();
    CHECK(bad.load() == 0);
}

void TestFormatAndHash()
{
    CHECK(std::format("{}", VarType(42)) == "42");
    CHECK(std::format("{}", VarType("hello")) == "hello");
    CHECK(std::format("[{:>6}]", VarType(42)) == "[    42]");
    CHECK(std::format("[{:<6}]", VarType("ab")) == "[ab    ]");
    CHECK(std::format("{} {}", VarType(1.5), VarType(true)) == "1.5 true");

    std::unordered_map<VarType, int> m;
    m[VarType("a")] = 1;
    m[VarType(5)] = 2;
    CHECK(m.at(VarType("a")) == 1 && m.at(VarType("5")) == 2 && m.count(VarType("b")) == 0);
    CHECK(std::hash<VarType>{}(VarType("x")) == std::hash<std::string_view>{}("x"));
}

} // namespace

int main()
{
    TestIntegerConstructionAndRoundTrip();
    TestFloatingRoundTripIsExact();
    TestNonFinite();
    TestBoolAndStringConstruction();
    TestNullCString();
    TestStrictConversion();
    TestIntegerAcceptsWholeDecimalForms();
    TestBoolConversion();
    TestAsAndExplicitConversions();
    TestComparisonWithNumbers();
    TestComparisonWithStringsAndVarType();
    TestNumericEqual();
    TestCopyMoveAndAssignment();
    TestConcurrentConstReads();
    TestFormatAndHash();
    std::puts("var_type_test: ok");
    return 0;
}
