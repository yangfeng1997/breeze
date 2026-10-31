// VarType：构造（所有整数宽度、浮点最短往返、被拒绝的类型）、严格转换与整数的"小数写法"、布尔、与数字 / 字符串的比较、
// 与原版行为的差别（精度丢失、垃圾得 0）、转换缓存（标签、命中、失败、跨类型、拷贝 / 移动 / 赋值）、容器（列表 / 映射的
// 格式、严格性、分隔符、随机往返）、格式化与哈希。
// 无测试框架依赖：失败即 abort。

// 测试按默认分隔符写：工程如果用宏覆盖了默认值，这里先取消，免得默认值相关的用例对不上。
#undef BREEZE_VARTYPE_ITEM_SEP
#undef BREEZE_VARTYPE_KV_SEP

#include "base/vartype/var_type.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <limits>
#include <map>
#include <random>
#include <string>
#include <unordered_map>
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

// VarType 声明的友元：白盒地看缓存的状态，并能直接往缓存里写值，证明"命中"确实没有重新解析。
namespace breeze
{
struct VarTypeTestPeer
{
    static std::uint8_t Tag(const VarType& v) { return v.cache_.tag; }
    static bool Ok(const VarType& v) { return v.cache_.ok; }
    template <typename T>
    static constexpr std::uint8_t TagOf()
    {
        return VarType::CacheTag<T>();
    }
    // 伪造一条缓存：下一次 TryAs<T> 应该原样返回它，而不是去解析文本。
    template <typename T>
    static void Poison(const VarType& v, std::optional<T> r)
    {
        v.cache_.Store<T>(TagOf<T>(), r);
    }
};
} // namespace breeze

namespace
{

using Peer = VarTypeTestPeer;

// 缓存标签：各类型互不相同且非零（串了就会拿 int 的缓存当 uint32_t 的结果），同宽同符号的共用，long double 不缓存。
constexpr std::uint8_t kAllTags[] = {Peer::TagOf<bool>(),
                                     Peer::TagOf<std::int8_t>(),
                                     Peer::TagOf<std::uint8_t>(),
                                     Peer::TagOf<std::int16_t>(),
                                     Peer::TagOf<std::uint16_t>(),
                                     Peer::TagOf<std::int32_t>(),
                                     Peer::TagOf<std::uint32_t>(),
                                     Peer::TagOf<std::int64_t>(),
                                     Peer::TagOf<std::uint64_t>(),
                                     Peer::TagOf<float>(),
                                     Peer::TagOf<double>()};
constexpr bool TagsDistinctAndNonZero()
{
    for (std::size_t i = 0; i < std::size(kAllTags); ++i)
    {
        if (kAllTags[i] == 0)
            return false;
        for (std::size_t j = i + 1; j < std::size(kAllTags); ++j)
            if (kAllTags[i] == kAllTags[j])
                return false;
    }
    return true;
}
static_assert(TagsDistinctAndNonZero());
static_assert(Peer::TagOf<long>() == Peer::TagOf<long long>() &&
              Peer::TagOf<unsigned long>() == Peer::TagOf<unsigned long long>() &&
              Peer::TagOf<int>() == Peer::TagOf<std::int32_t>() && Peer::TagOf<const int>() == Peer::TagOf<int>());
static_assert(Peer::TagOf<long double>() == 0);

// 默认分隔符：常量与 var_detail::ValidSeparator 的判定（运行时的用例见 TestVectorSeparators / TestMapSeparators）
static_assert(kVarItemSep == "|" && kVarKvSep == "=");
static_assert(var_detail::ValidSeparator("|") && var_detail::ValidSeparator("=>") && var_detail::ValidSeparator("·") &&
              var_detail::ValidSeparator(";") && var_detail::ValidSeparator("x"));
static_assert(!var_detail::ValidSeparator("") && !var_detail::ValidSeparator("1") && !var_detail::ValidSeparator("-") &&
              !var_detail::ValidSeparator("+") && !var_detail::ValidSeparator(".") &&
              !var_detail::ValidSeparator("e") && !var_detail::ValidSeparator("E") &&
              !var_detail::ValidSeparator(" ") && !var_detail::ValidSeparator("\t") &&
              !var_detail::ValidSeparator("a b") && !var_detail::ValidSeparator("x9"));

constexpr bool EveryDigitIsInvalidSeparator()
{
    for (char c = '0'; c <= '9'; ++c)
        if (var_detail::ValidSeparator(std::string_view(&c, 1)))
            return false;
    return true;
}
static_assert(EveryDigitIsInvalidSeparator());

// 容器的元素：数字、bool、std::string 可以；字符类、string_view（会悬空）、容器本身不行
template <typename T>
concept CanVector = requires(const VarType& v) { v.template TryAsVector<T>(); };
template <typename K, typename V>
concept CanMap = requires(const VarType& v) { v.template TryAsMap<K, V>(); };
static_assert(CanVector<int> && CanVector<std::uint8_t> && CanVector<double> && CanVector<bool> &&
              CanVector<std::string>);
static_assert(!CanVector<char> && !CanVector<wchar_t> && !CanVector<std::string_view> && !CanVector<const char*> &&
              !CanVector<std::vector<int>>);
static_assert(CanMap<int, int> && CanMap<std::string, double> && CanMap<std::string, std::string>);
static_assert(!CanMap<char, int> && !CanMap<int, std::string_view> && !CanMap<std::vector<int>, int>);

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

// 缓存的填充、命中、覆盖、失败。命中用 Poison 证明：往缓存里塞一个和文本对不上的值，转换必须原样取出它。
void TestCacheFillHitAndFailure()
{
    const VarType v = "42";
    CHECK(Peer::Tag(v) == 0);
    CHECK(v.As<int>() == 42);
    CHECK(Peer::Tag(v) == Peer::TagOf<int>() && Peer::Ok(v));

    Peer::Poison<int>(v, 7);
    CHECK(v.As<int>() == 7 && v.TryAs<int>() == 7 && v == 7); // 命中：没有重新解析
    Peer::Poison<int>(v, std::nullopt);
    CHECK(!v.TryAs<int>() && v.As<int>(-1) == -1 && !(v == 42)); // 失败的结果也命中
    CHECK(Peer::Tag(v) == Peer::TagOf<int>() && !Peer::Ok(v));

    // 换类型：单槽被覆盖，文本没变，所以新类型按文本解析；转回来再按文本解析
    CHECK(v.As<double>() == 42.0);
    CHECK(Peer::Tag(v) == Peer::TagOf<double>() && Peer::Ok(v));
    CHECK(v.As<int>() == 42); // 之前毒化的失败已被覆盖
    CHECK(Peer::Tag(v) == Peer::TagOf<int>());

    // 真实的失败：转换失败时记下失败，再转仍然失败，也不影响别的类型
    const VarType bad = "abc";
    CHECK(!bad.TryAs<int>());
    CHECK(Peer::Tag(bad) == Peer::TagOf<int>() && !Peer::Ok(bad));
    CHECK(!bad.TryAs<int>() && bad.As<int>(-1) == -1 && !bad.TryAs<double>());
    const VarType t = "true";
    CHECK(!t.TryAs<int>() && t.As<bool>() && !t.TryAs<int>() && t.As<bool>());

    // 不缓存的类型：long double 每次都解析，缓存槽不动
    const VarType w = "2.5";
    CHECK(w.As<int>() == 2 || !w.TryAs<int>()); // 先占住槽
    const auto tag_before = Peer::Tag(w);
    CHECK(w.As<long double>() == 2.5L && w.As<long double>() == 2.5L);
    CHECK(Peer::Tag(w) == tag_before);

    // 命中走的是 const 对象的 mutable 缓存：对 const 引用转换也能填充
    const VarType& cref = w;
    CHECK(cref.As<double>() == 2.5 && Peer::Tag(w) == Peer::TagOf<double>());
}

// 缓存永远不能把一个类型的结果当成另一个类型的：任意两个类型先后转换，结果都等于全新对象的转换结果。
template <typename A, typename B>
void ExpectCrossType(const char* s)
{
    const VarType warm(s);
    (void)warm.TryAs<A>();
    CHECK(warm.TryAs<B>() == VarType(s).TryAs<B>());
    CHECK(warm.TryAs<B>() == VarType(s).TryAs<B>()); // 第二次是命中
    CHECK(warm.TryAs<A>() == VarType(s).TryAs<A>());
    CHECK(warm.TryAs<A>() == VarType(s).TryAs<A>());
}
template <typename A, typename... Bs>
void ExpectFrom(const char* s)
{
    (ExpectCrossType<A, Bs>(s), ...);
}
template <typename... Ts>
void ExpectAllPairs(const char* s)
{
    (ExpectFrom<Ts, Ts...>(s), ...);
}

void TestCacheNeverServesWrongType()
{
    const char* samples[] = {"0",
                             "1",
                             "-1",
                             "42",
                             "255",
                             "256",
                             "-128",
                             "-129",
                             "300",
                             "65535",
                             "70000",
                             "2147483647",
                             "3000000000",
                             "-3000000000",
                             "5000000000",
                             "9007199254740993",
                             "18446744073709551615",
                             "0.1",
                             "2.5",
                             "1e3",
                             "5.0",
                             " 42 ",
                             "+7",
                             "true",
                             "off",
                             "abc",
                             ""};
    for (const char* s : samples)
    {
        ExpectAllPairs<bool,
                       std::int8_t,
                       std::uint8_t,
                       std::int16_t,
                       std::uint16_t,
                       std::int32_t,
                       std::uint32_t,
                       std::int64_t,
                       std::uint64_t,
                       float,
                       double,
                       long double>(s);
    }
    // 同宽同符号共用一个标签：long 与 long long，转一个再转另一个，值对得上
    const VarType v = "-9000000000";
    CHECK(v.As<long>() == -9000000000L && v.As<long long>() == -9000000000LL && v.As<std::int64_t>() == -9000000000LL);
    CHECK(Peer::Tag(v) == Peer::TagOf<long>());
}

// 拷贝带着缓存走；移动后源对象的缓存清空（它的文本被搬走了，缓存不能还报着旧值）；赋值用新对象的缓存覆盖旧的。
void TestCacheFollowsCopyMoveAssign()
{
    VarType a = "42";
    CHECK(a.As<int>() == 42);

    VarType b = a; // 拷贝构造
    CHECK(Peer::Tag(b) == Peer::TagOf<int>() && Peer::Tag(a) == Peer::TagOf<int>());
    CHECK(b.As<int>() == 42 && a.As<int>() == 42);

    VarType c = std::move(a); // 移动构造：目标保留，源清空
    CHECK(Peer::Tag(c) == Peer::TagOf<int>() && c.As<int>() == 42);
    CHECK(Peer::Tag(a) == 0);                                            // NOLINT
    CHECK(a.TryAs<int>() == VarType(std::string(a.str())).TryAs<int>()); // NOLINT 与它现在的文本一致

    VarType d = "1";
    CHECK(d.As<int>() == 1);
    d = VarType("7"); // 移动赋值：被覆盖，不能留着旧值
    CHECK(Peer::Tag(d) == 0 && d.As<int>() == 7);

    VarType e = "8";
    CHECK(e.As<int>() == 8);
    VarType f = "9";
    CHECK(f.As<double>() == 9.0);
    f = e; // 拷贝赋值：缓存跟着 e 走
    CHECK(Peer::Tag(f) == Peer::TagOf<int>() && f.As<int>() == 8);
    VarType g = "5";
    CHECK(g.As<int>() == 5);
    VarType h = "6"; // 来源没有缓存
    g = h;
    CHECK(Peer::Tag(g) == 0 && g.As<int>() == 6);

    VarType m = "11";
    CHECK(m.As<int>() == 11);
    VarType n = "12";
    n = std::move(m); // 移动赋值：目标得到缓存，源清空
    CHECK(Peer::Tag(n) == Peer::TagOf<int>() && n.As<int>() == 11 && Peer::Tag(m) == 0); // NOLINT

    // 自赋值（拷贝）后仍然对；自移动赋值后与文本一致（缓存可以被清掉）
    VarType s = "21";
    CHECK(s.As<int>() == 21);
    VarType& alias = s;
    s = alias;
    CHECK(s.As<int>() == 21 && Peer::Tag(s) == Peer::TagOf<int>());
    s = std::move(alias);
    CHECK(s.TryAs<int>() == VarType(std::string(s.str())).TryAs<int>());
}

// 缓存只是加速：==、hash、format、NumericEqual 都只看文本。
void TestCacheDoesNotAffectComparison()
{
    const VarType a = "42";
    const VarType b = "42";
    CHECK(a.As<int>() == 42 && b.As<double>() == 42.0); // 两边缓存不同
    CHECK(a == b && a == "42" && std::hash<VarType>{}(a) == std::hash<VarType>{}(b));
    CHECK(std::format("{}", a) == "42" && std::format("{}", b) == "42");
    CHECK(a.NumericEqual(b) && b.NumericEqual(a));
    const VarType c = "42.0";
    CHECK(c.As<int>() == 42 && !(a == c) && a.NumericEqual(c));
}

// ---- 容器：TryAsVector / TryAsMap ----

void TestVectorFormatAndElementRules()
{
    using IV = std::vector<int>;
    CHECK(VarType("1|2|3").TryAsVector<int>() == IV({1, 2, 3}));
    CHECK(VarType("5").TryAsVector<int>() == IV({5}));
    // 元素规则与 TryAs 一致：正负号、整数的小数写法、科学计数法
    CHECK(VarType("-1|+2|3.0|1e2").TryAsVector<int>() == IV({-1, 2, 3, 100}));
    CHECK(VarType("1.5|-2|3e-1").TryAsVector<double>() == std::vector<double>({1.5, -2.0, 0.3}));
    CHECK(VarType("true|0|YES|off").TryAsVector<bool>() == std::vector<bool>({true, false, true, false}));
    CHECK(VarType("a|b c|d").TryAsVector<std::string>() == std::vector<std::string>({"a", "b c", "d"}));
    // 元素两端的空白去掉（数字、字符串一样）
    CHECK(VarType(" 1 |\t2\n| 3 ").TryAsVector<int>() == IV({1, 2, 3}));
    CHECK(VarType(" a | b ").TryAsVector<std::string>() == std::vector<std::string>({"a", "b"}));

    // 整串为空或只有空白：空容器，不是失败
    for (const char* s : {"", "   ", "\t\n"})
    {
        const auto r = VarType(s).TryAsVector<int>();
        CHECK(r.has_value() && r->empty());
    }

    // 任何一个元素转换失败，整体失败
    for (const char* s : {"1|a|3", "1|2.5|3", "1|2 3", "abc", "1,2", "1|0x10"})
        CHECK(!VarType(s).TryAsVector<int>());
    CHECK(VarType("1|255").TryAsVector<std::uint8_t>().has_value() && !VarType("1|256").TryAsVector<std::uint8_t>());
    CHECK(!VarType("0|-1").TryAsVector<std::uint32_t>() && !VarType("1|x").TryAsVector<bool>());
}

void TestVectorEmptyItems()
{
    using IV = std::vector<int>;
    // 空项默认失败：尾部多一个分隔符几乎一定是笔误；只有空白的项也算空
    for (const char* s : {"1||3", "1|2|", "|1", "|", "||", "1| |3", " |1"})
        CHECK(!VarType(s).TryAsVector<int>());
    CHECK(!VarType("a||b").TryAsVector<std::string>() && !VarType("a|").TryAsVector<std::string>());

    // skip_empty：跳过空项，但不放过真正的坏元素
    CHECK(VarType("1||3").TryAsVector<int>("|", true) == IV({1, 3}));
    CHECK(VarType("1|2|").TryAsVector<int>("|", true) == IV({1, 2}));
    CHECK(VarType("|1").TryAsVector<int>("|", true) == IV({1}));
    CHECK(VarType("1| |3").TryAsVector<int>("|", true) == IV({1, 3}));
    const auto none = VarType("|||").TryAsVector<int>("|", true);
    CHECK(none.has_value() && none->empty());
    CHECK(!VarType("1|x|3").TryAsVector<int>("|", true));
}

void TestVectorSeparators()
{
    using IV = std::vector<int>;
    using SV = std::vector<std::string>;
    CHECK(VarType("1:2:3").TryAsVector<int>(":") == IV({1, 2, 3}));
    CHECK(VarType("1;2;3").TryAsVector<int>(";") == IV({1, 2, 3}));
    CHECK(VarType("1,2,3").TryAsVector<int>(",") == IV({1, 2, 3}));
    CHECK(VarType("1x2x3").TryAsVector<int>("x") == IV({1, 2, 3}));
    CHECK(VarType("1::2::3").TryAsVector<int>("::") == IV({1, 2, 3}));
    CHECK(VarType("1=>2=>3").TryAsVector<int>("=>") == IV({1, 2, 3}));
    CHECK(!VarType("1::2").TryAsVector<int>(":")); // 单个分隔符切出空项

    // 整串匹配：多字符分隔符里的单个字符不算；多字节分隔符不会按字节切
    // （'·' 是 C2 B7，"路" 是 E8 B7 AF，里面有 B7 字节）
    CHECK(VarType("1|2").TryAsVector<std::string>("||") == SV({"1|2"}));
    CHECK(VarType("1||2").TryAsVector<int>("||") == IV({1, 2}));
    CHECK(VarType("路·x·中").TryAsVector<std::string>("·") == SV({"路", "x", "中"}));

    // 非法分隔符：空、含数字或 "+-.eE"、含空白。无论元素类型、文本是否为空，一律失败
    for (const char* sep : {"", "1", "9", "-", "+", ".", "e", "E", " ", "\t", "|1", "a e", "x-y", "12", "\n"})
    {
        CHECK(!VarType("1|2").TryAsVector<int>(sep));
        CHECK(!VarType("a|b").TryAsVector<std::string>(sep));
        CHECK(!VarType("").TryAsVector<std::string>(sep));
    }
}

void TestVectorRoundTripRandom()
{
    std::mt19937_64 rng(20240607);
    for (int round = 0; round < 3000; ++round)
    {
        std::vector<std::int64_t> ints(rng() % 8); // 可能为空
        std::vector<double> doubles(rng() % 6);
        std::string itext;
        std::string dtext;
        for (std::size_t i = 0; i < ints.size(); ++i)
        {
            ints[i] = static_cast<std::int64_t>(rng()) >> (rng() % 64); // 大小各种量级、正负都有
            itext += (i != 0 ? (rng() % 2 ? "|" : " | ") : "") + std::to_string(ints[i]);
        }
        for (std::size_t i = 0; i < doubles.size(); ++i)
        {
            double d;
            do
                d = std::bit_cast<double>(rng());
            while (!std::isfinite(d));
            doubles[i] = d;
            dtext += (i != 0 ? "|" : "") + VarType(d).str();
        }
        CHECK(VarType(itext).TryAsVector<std::int64_t>() == ints);
        CHECK(VarType(dtext).TryAsVector<double>() == doubles);
    }
}

void TestMapFormatAndElementRules()
{
    using II = std::map<int, int>;
    CHECK(VarType("12=34|45=78").TryAsMap<int, int>() == II({{12, 34}, {45, 78}}));
    CHECK(VarType("7=8").TryAsMap<int, int>() == II({{7, 8}}));
    // std::map 是有序的，与输入顺序无关
    const auto m = VarType("45=1|12=2|30=3").TryAsMap<int, int>();
    CHECK(m.has_value() && m->begin()->first == 12 && m->rbegin()->first == 45 && m->size() == 3);
    // 键、值两端的空白去掉
    CHECK(VarType(" 12 = 34 | 45=78 ").TryAsMap<int, int>() == II({{12, 34}, {45, 78}}));
    // 键与值可以是不同类型
    CHECK(VarType("atk=100|def=50").TryAsMap<std::string, int>() ==
          std::map<std::string, int>({{"atk", 100}, {"def", 50}}));
    CHECK(VarType("a=x y|b=z").TryAsMap<std::string, std::string>() ==
          std::map<std::string, std::string>({{"a", "x y"}, {"b", "z"}}));
    CHECK(VarType("1=0.5|2=1e1").TryAsMap<int, double>() == std::map<int, double>({{1, 0.5}, {2, 10.0}}));
    CHECK(VarType("1=true|2=off").TryAsMap<int, bool>() == std::map<int, bool>({{1, true}, {2, false}}));
    // 键按元素类型比较：int 的 "1" 与 "01" 是同一个键，字符串的不是
    CHECK(VarType("1=1|01=2").TryAsMap<std::string, int>() == std::map<std::string, int>({{"01", 2}, {"1", 1}}));

    for (const char* s : {"", "   ", "\t\n"})
    {
        const auto r = VarType(s).TryAsMap<int, int>();
        CHECK(r.has_value() && r->empty());
    }
}

void TestMapStrictness()
{
    // 缺键值分隔符、多于一个、键或值为空、元素转换失败、键重复（含 "1" 与 "01"）、空项
    for (const char* s : {"12|45=78",
                          "12=34=5",
                          "1==2",
                          "=34",
                          "12=",
                          "= ",
                          "a=1",
                          "1=x",
                          "1=1|1=2",
                          "1=1|01=2",
                          "1=2|",
                          "|1=2",
                          "1=2||3=4",
                          "1=2| |3=4",
                          "1.5=2"})
        CHECK(!VarType(s).TryAsMap<int, int>());
    CHECK(!VarType("a=|b=2").TryAsMap<std::string, std::string>() &&
          !VarType("=a|b=2").TryAsMap<std::string, std::string>());
    // 值是字符串时，多一个键值分隔符不会被当成值的一部分
    CHECK(!VarType("a=b=c").TryAsMap<std::string, std::string>() &&
          !VarType("a==b").TryAsMap<std::string, std::string>());

    // skip_empty 只跳过整个空项，缺分隔符与键重复照样失败
    using II = std::map<int, int>;
    CHECK(VarType("1=2||3=4").TryAsMap<int, int>("|", "=", true) == II({{1, 2}, {3, 4}}));
    CHECK(VarType("1=2|").TryAsMap<int, int>("|", "=", true) == II({{1, 2}}));
    CHECK(VarType("| 1=2").TryAsMap<int, int>("|", "=", true) == II({{1, 2}}));
    CHECK(VarType("1=2| |3=4").TryAsMap<int, int>("|", "=", true) == II({{1, 2}, {3, 4}}));
    CHECK(!VarType("1|3=4").TryAsMap<int, int>("|", "=", true) &&
          !VarType("1=2|1=3").TryAsMap<int, int>("|", "=", true));
}

void TestMapSeparators()
{
    using II = std::map<int, int>;
    using SI = std::map<std::string, int>;
    CHECK(VarType("12:34,45:78").TryAsMap<int, int>(",", ":") == II({{12, 34}, {45, 78}}));
    CHECK(VarType("a=>1&b=>2").TryAsMap<std::string, int>("&", "=>") == SI({{"a", 1}, {"b", 2}}));
    CHECK(VarType("a·1|b·2").TryAsMap<std::string, int>("|", "·") == SI({{"a", 1}, {"b", 2}}));
    CHECK(VarType("路·1;中·2").TryAsMap<std::string, int>(";", "·") == SI({{"中", 2}, {"路", 1}}));
    CHECK(!VarType("a=1").TryAsMap<std::string, int>("|", "=>")); // 整串匹配：单个 '=' 不是 "=>"
    // 项分隔符同样整串匹配：多字符的 "||" 不会按单个 '|' 切；多字节的 "·" 不会按字节切（"路" 里有 B7 字节）
    CHECK(VarType("a=1||b=2").TryAsMap<std::string, int>("||", "=") == SI({{"a", 1}, {"b", 2}}));
    // 单个 '|' 不是 "||"：整串是一项，里面有两个 '='，失败
    CHECK(!VarType("a=1|b=2").TryAsMap<std::string, std::string>("||", "="));
    CHECK(VarType("路=1·中=2").TryAsMap<std::string, int>("·", "=") == SI({{"中", 2}, {"路", 1}}));
    // 多字符键值分隔符：重叠出现也算多个（"a===b" 里 "==" 出现在位置 1 和 2），有歧义，失败
    CHECK(VarType("a==b").TryAsMap<std::string, std::string>("|", "==") ==
          std::map<std::string, std::string>({{"a", "b"}}));
    CHECK(!VarType("a===b").TryAsMap<std::string, std::string>("|", "=="));

    // 两级分隔符相同：无法区分，失败
    CHECK(!VarType("1=2").TryAsMap<int, int>("|", "|") && !VarType("1=2").TryAsMap<int, int>("=", "="));
    CHECK(!VarType("").TryAsMap<int, int>("|", "|") && !VarType("|").TryAsMap<int, int>("|", "|", true));
    // 非法分隔符：任何一级、任何文本（包括空文本）都失败
    for (const char* bad : {"", "1", "-", "+", ".", "e", "E", " ", "x1"})
    {
        CHECK(!VarType("1=2").TryAsMap<int, int>(bad, "="));
        CHECK(!VarType("1=2").TryAsMap<int, int>("|", bad));
        CHECK(!VarType("").TryAsMap<int, int>(bad, "="));
        CHECK(!VarType("").TryAsMap<int, int>("|", bad));
    }
}

void TestMapRoundTripRandom()
{
    std::mt19937_64 rng(7654321);
    for (int round = 0; round < 3000; ++round)
    {
        std::map<std::int32_t, std::int32_t> src;
        const std::size_t n = rng() % 8;
        for (std::size_t i = 0; i < n; ++i)
            src[static_cast<std::int32_t>(rng()) >> (rng() % 32)] = static_cast<std::int32_t>(rng());
        std::string text;
        for (const auto& [k, v] : src)
            text += (text.empty() ? "" : "|") + std::to_string(k) + "=" + std::to_string(v);
        CHECK((VarType(text).TryAsMap<std::int32_t, std::int32_t>() == src));
    }
}

// 容器转换不碰转换缓存：缓存里的标量结果原样保留。
void TestContainersDoNotTouchCache()
{
    const VarType v = "1|2";
    CHECK(!v.TryAs<int>() && Peer::Tag(v) == Peer::TagOf<int>() && !Peer::Ok(v));
    CHECK(v.TryAsVector<int>() == std::vector<int>({1, 2}));
    CHECK(Peer::Tag(v) == Peer::TagOf<int>() && !Peer::Ok(v));
    const VarType m = "1=2";
    CHECK(m.As<double>(-1.0) == -1.0 && Peer::Tag(m) == Peer::TagOf<double>());
    CHECK(m.TryAsMap<int, int>() == std::map<int, int>({{1, 2}}));
    CHECK(Peer::Tag(m) == Peer::TagOf<double>() && !Peer::Ok(m));
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
    TestCacheFillHitAndFailure();
    TestCacheNeverServesWrongType();
    TestCacheFollowsCopyMoveAssign();
    TestCacheDoesNotAffectComparison();
    TestVectorFormatAndElementRules();
    TestVectorEmptyItems();
    TestVectorSeparators();
    TestVectorRoundTripRandom();
    TestMapFormatAndElementRules();
    TestMapStrictness();
    TestMapSeparators();
    TestMapRoundTripRandom();
    TestContainersDoNotTouchCache();
    TestFormatAndHash();
    std::puts("var_type_test: ok");
    return 0;
}
