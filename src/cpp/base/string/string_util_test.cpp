// string_util：边界用例 + 与朴素参考实现的随机对照。无测试框架依赖：失败即 abort。

#include "base/string/string_util.h"

#include <clocale>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <random>
#include <string>
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

using Views = std::vector<std::string_view>;

// 传入临时 std::string 必须编译不过：结果会悬空。requires 表达式要放在模板里才是"可替换失败"，不是硬错误。
template <typename S>
concept CanSplitChar = requires(S&& s) { str::Split(std::forward<S>(s), ','); };
template <typename S>
concept CanSplitStr = requires(S&& s) { str::Split(std::forward<S>(s), std::string_view(",")); };
template <typename S>
concept CanSplitAny = requires(S&& s) { str::SplitAny(std::forward<S>(s), ","); };
template <typename S>
concept CanTrim = requires(S&& s) { str::Trim(std::forward<S>(s)); };
template <typename S>
concept CanTrimLeft = requires(S&& s) { str::TrimLeft(std::forward<S>(s)); };
template <typename S>
concept CanTrimRight = requires(S&& s) { str::TrimRight(std::forward<S>(s)); };

static_assert(!CanSplitChar<std::string>);
static_assert(!CanSplitStr<std::string>);
static_assert(!CanSplitAny<std::string>);
static_assert(!CanTrim<std::string>);
static_assert(!CanTrimLeft<std::string>);
static_assert(!CanTrimRight<std::string>);
// 左值、字面量、string_view 都得能用
static_assert(CanSplitChar<std::string&> && CanSplitChar<const std::string&> && CanSplitChar<const char (&)[4]> &&
              CanSplitChar<std::string_view> && CanSplitChar<const char*>);
static_assert(CanSplitStr<std::string&> && CanSplitStr<const char (&)[4]>);
static_assert(CanSplitAny<std::string&> && CanSplitAny<const char (&)[4]>);
static_assert(CanTrim<std::string&> && CanTrim<const char (&)[4]> && CanTrim<std::string_view>);

void TestSplitChar()
{
    CHECK((str::Split("a,b,c", ',') == Views{"a", "b", "c"}));
    CHECK((str::Split("", ',') == Views{""})); // 空串得到一个空段
    CHECK((str::Split("", ',', true) == Views{}));
    CHECK((str::Split(",", ',') == Views{"", ""}));
    CHECK((str::Split("a,,b", ',') == Views{"a", "", "b"}));
    CHECK((str::Split("a,,b", ',', true) == Views{"a", "b"}));
    CHECK((str::Split(",a,", ',') == Views{"", "a", ""}));
    CHECK((str::Split(",a,", ',', true) == Views{"a"}));
    CHECK((str::Split("abc", ',') == Views{"abc"}));
    CHECK((str::Split(",,,", ',', true) == Views{}));

    // 结果指向入参的内存，不拷贝
    const std::string s = "xx,yy";
    const auto v = str::Split(s, ',');
    CHECK(v[0].data() == s.data() && v[1].data() == s.data() + 3);

    // 嵌入的 NUL 不是结束符
    const std::string nul("a\0b,c", 5);
    const auto nv = str::Split(nul, ',');
    CHECK(nv.size() == 2 && nv[0].size() == 3);
}

void TestSplitString()
{
    CHECK((str::Split("a::b::c", "::") == Views{"a", "b", "c"}));
    CHECK((str::Split("a:::b", "::") == Views{"a", ":b"})); // 从左到右不重叠
    CHECK((str::Split("::a::", "::") == Views{"", "a", ""}));
    CHECK((str::Split("::a::", "::", true) == Views{"a"}));
    CHECK((str::Split("abc", "") == Views{"abc"})); // 空分隔符：不分割
    CHECK((str::Split("", "") == Views{""}));
    CHECK((str::Split("", "", true) == Views{}));
    CHECK((str::Split("abc", "abc") == Views{"", ""}));
    CHECK((str::Split("ab", "abc") == Views{"ab"})); // 分隔符比原串长
}

void TestSplitAny()
{
    CHECK((str::SplitAny("a, b;c", " ,;") == Views{"a", "", "b", "c"}));
    CHECK((str::SplitAny("a, b;c", " ,;", true) == Views{"a", "b", "c"}));
    CHECK((str::SplitAny("abc", "") == Views{"abc"})); // 没有分隔字符
    CHECK((str::SplitAny("", ",") == Views{""}));
    CHECK((str::SplitAny("", ",", true) == Views{}));
}

void TestSplitOwned()
{
    std::vector<std::string> v;
    {
        std::string tmp = "p,q";
        v = str::SplitOwned(tmp, ',');
    } // tmp 已销毁
    CHECK((v == std::vector<std::string>{"p", "q"}));
}

void TestJoin()
{
    CHECK(str::Join(std::vector<std::string>{"a", "b", "c"}, ", ") == "a, b, c");
    CHECK(str::Join(std::vector<std::string>{}, ",").empty());
    CHECK(str::Join(std::vector<std::string>{"x"}, ",") == "x");
    CHECK(str::Join(std::vector<std::string>{"", ""}, ",") == ",");
    CHECK(str::Join(Views{"a", "b"}, "") == "ab");
    CHECK(str::Join({"p", "q"}, "-") == "p-q");
    const char* arr[] = {"u", "v", "w"};
    CHECK(str::Join(arr, "/") == "u/v/w");
}

// 随机串（字母表小，重复分隔符多）：Join(Split(s)) 还原 s；与朴素实现逐段一致
std::string RandomString(std::mt19937& rng, std::string_view alphabet, std::size_t max_len)
{
    std::string s(rng() % (max_len + 1), ' ');
    for (char& c : s)
        c = alphabet[rng() % alphabet.size()];
    return s;
}

Views NaiveSplit(std::string_view s, std::string_view delim, bool skip_empty)
{
    Views out;
    std::size_t i = 0, seg = 0;
    while (i + delim.size() <= s.size())
    {
        if (s.substr(i, delim.size()) == delim)
        {
            if (!skip_empty || i > seg)
                out.push_back(s.substr(seg, i - seg));
            i += delim.size();
            seg = i;
        }
        else
        {
            ++i;
        }
    }
    if (!skip_empty || s.size() > seg)
        out.push_back(s.substr(seg));
    return out;
}

void TestSplitRandomized()
{
    std::mt19937 rng(12345);
    for (int iter = 0; iter < 20000; ++iter)
    {
        const std::string s = RandomString(rng, "ab,;", 14);
        // 字符分隔 + 还原
        const auto parts = str::Split(s, ',');
        CHECK(str::Join(parts, ",") == s);
        CHECK(parts == NaiveSplit(s, ",", false));
        CHECK(str::Split(s, ',', true) == NaiveSplit(s, ",", true));
        // 子串分隔
        const std::string delim = RandomString(rng, "ab,", 3);
        if (!delim.empty())
        {
            CHECK(str::Split(s, std::string_view(delim)) == NaiveSplit(s, delim, false));
            CHECK(str::Split(s, std::string_view(delim), true) == NaiveSplit(s, delim, true));
            CHECK(str::Join(str::Split(s, std::string_view(delim)), delim) == s);
        }
    }
}

void TestTrim()
{
    CHECK(str::Trim("  a b  ") == "a b");
    CHECK(str::Trim("\t\r\n\v\f x \t") == "x");
    CHECK(str::Trim("   ").empty());
    CHECK(str::Trim("").empty());
    CHECK(str::TrimLeft("  a  ") == "a  ");
    CHECK(str::TrimLeft("   ").empty()); // 全是空白：不能把原串还回去
    CHECK(str::TrimRight("   ").empty());
    CHECK(str::TrimLeft("").empty() && str::TrimRight("").empty());
    CHECK(str::TrimRight("  a  ") == "  a");
    CHECK(str::Trim("xxaxx", "x") == "a");
    CHECK(str::Trim("xxxx", "x").empty());
    CHECK(str::Trim("a") == "a");
    CHECK(str::Trim("\xE3\x80\x80x") == "\xE3\x80\x80x"); // UTF-8 全角空格不是 ASCII 空白，不动
    // 视图指向原内存
    const std::string s = "  ab ";
    CHECK(str::Trim(s).data() == s.data() + 2);
}

void TestCase()
{
    CHECK(str::ToUpper("Hello, World 123") == "HELLO, WORLD 123");
    CHECK(str::ToLower("Hello, World 123") == "hello, world 123");
    CHECK(str::ToUpper("").empty());
    // 非 ASCII 字节（>= 0x80）原样保留：中文、带音标字母
    CHECK(str::ToUpper("a\xE4\xB8\xAD\xC3\xA9") == "A\xE4\xB8\xAD\xC3\xA9");
    CHECK(str::ToLower("A\xE4\xB8\xAD\xC3\x89") == "a\xE4\xB8\xAD\xC3\x89");

    // 256 个字节值逐个对照：只有 a-z / A-Z 变，边界（'`' '{' '@' '['）与高位字节都不动
    for (int b = 0; b < 256; ++b)
    {
        const char c = static_cast<char>(b);
        const char up = (b >= 'a' && b <= 'z') ? static_cast<char>(b - 32) : c;
        const char low = (b >= 'A' && b <= 'Z') ? static_cast<char>(b + 32) : c;
        CHECK(str::ToUpper(std::string(1, c)) == std::string(1, up));
        CHECK(str::ToLower(std::string(1, c)) == std::string(1, low));
        CHECK(str::EqualsIgnoreCase(std::string_view(&c, 1), std::string_view(&up, 1)));
    }

    std::string s = "aBc";
    str::ToUpperInPlace(s);
    CHECK(s == "ABC");
    str::ToLowerInPlace(s);
    CHECK(s == "abc");

    CHECK(str::EqualsIgnoreCase("Hello", "hELLO"));
    CHECK(!str::EqualsIgnoreCase("Hello", "Hell"));
    CHECK(!str::EqualsIgnoreCase("a", "b"));
    CHECK(str::EqualsIgnoreCase("", ""));
    CHECK(!str::EqualsIgnoreCase("\xC3\xA9", "\xC3\x89")); // 不做 Unicode 折叠

    // 与 locale 无关：土耳其语 locale 下 toupper('i') 可能不是 'I'
    if (std::setlocale(LC_ALL, "tr_TR.UTF-8") != nullptr || std::setlocale(LC_ALL, "tr_TR.utf8") != nullptr)
    {
        CHECK(str::ToUpper("title") == "TITLE");
        CHECK(str::ToLower("TITLE") == "title");
        std::setlocale(LC_ALL, "C");
    }
}

std::string NaiveReplace(std::string_view s, std::string_view from, std::string_view to, std::size_t max_count)
{
    std::string out;
    std::size_t i = 0, done = 0;
    while (i < s.size())
    {
        if (!from.empty() && done < max_count && s.substr(i, from.size()) == from)
        {
            out.append(to);
            i += from.size();
            ++done;
        }
        else
        {
            out += s[i++];
        }
    }
    return out;
}

void TestReplace()
{
    CHECK(str::ReplaceAll("a-b-c", "-", "+") == "a+b+c");
    CHECK(str::ReplaceAll("abc", "x", "y") == "abc");
    CHECK(str::ReplaceAll("abc", "", "y") == "abc"); // from 为空：原样
    CHECK(str::ReplaceAll("", "a", "b").empty());
    CHECK(str::ReplaceAll("aaa", "aa", "b") == "ba");           // 从左到右不重叠
    CHECK(str::ReplaceAll("aaaa", "aa", "a") == "aa");          // 结果不再被扫描
    CHECK(str::ReplaceAll("abab", "ab", "abab") == "abababab"); // to 含 from 不会死循环
    CHECK(str::ReplaceAll("a.b.c", ".", "") == "abc");
    CHECK(str::Replace("a-b-c-d", "-", "+", 2) == "a+b+c-d");
    CHECK(str::Replace("a-b", "-", "+", 0) == "a-b");
    CHECK(str::Replace("a-b", "-", "+", 100) == "a+b");

    std::mt19937 rng(777);
    for (int iter = 0; iter < 20000; ++iter)
    {
        const std::string s = RandomString(rng, "ab", 12);
        const std::string from = RandomString(rng, "ab", 3);
        const std::string to = RandomString(rng, "abc", 4);
        const std::size_t max_count = rng() % 4 == 0 ? rng() % 3 : std::string_view::npos;
        CHECK(str::Replace(s, from, to, max_count) == NaiveReplace(s, from, to, max_count));
    }
}

void TestRepeat()
{
    CHECK(str::Repeat("ab", 3) == "ababab");
    CHECK(str::Repeat("ab", 0).empty());
    CHECK(str::Repeat("", 5).empty());
    CHECK(str::Repeat("x", 1) == "x");
}

template <typename T>
void ExpectParse(std::string_view s, T expected, int base = 10)
{
    const auto v = str::ParseNumber<T>(s, base);
    CHECK(v.has_value() && *v == expected);
}

template <typename T>
void ExpectFail(std::string_view s, int base = 10)
{
    CHECK(!str::ParseNumber<T>(s, base).has_value());
}

void TestParseInteger()
{
    ExpectParse<int>("0", 0);
    ExpectParse<int>("-0", 0);
    ExpectParse<int>("123", 123);
    ExpectParse<int>("-123", -123);
    ExpectParse<int>("007", 7);

    // 各宽度的边界与溢出
    ExpectParse<std::int8_t>("127", 127);
    ExpectParse<std::int8_t>("-128", -128);
    ExpectFail<std::int8_t>("128");
    ExpectFail<std::int8_t>("-129");
    ExpectParse<std::uint8_t>("255", 255);
    ExpectFail<std::uint8_t>("256");
    ExpectParse<std::int32_t>("2147483647", std::numeric_limits<std::int32_t>::max());
    ExpectParse<std::int32_t>("-2147483648", std::numeric_limits<std::int32_t>::min());
    ExpectFail<std::int32_t>("2147483648");
    ExpectParse<std::int64_t>("9223372036854775807", std::numeric_limits<std::int64_t>::max());
    ExpectParse<std::int64_t>("-9223372036854775808", std::numeric_limits<std::int64_t>::min());
    ExpectFail<std::int64_t>("9223372036854775808");
    ExpectParse<std::uint64_t>("18446744073709551615", std::numeric_limits<std::uint64_t>::max());
    ExpectFail<std::uint64_t>("18446744073709551616");

    // 无符号不接受负号
    ExpectFail<std::uint32_t>("-1");
    ExpectFail<std::uint64_t>("-1");

    // 严格：整串必须恰好是一个数
    ExpectFail<int>("");
    ExpectFail<int>("-");
    ExpectFail<int>("+5");
    ExpectFail<int>(" 5");
    ExpectFail<int>("5 ");
    ExpectFail<int>("12abc");
    ExpectFail<int>("abc");
    ExpectFail<int>("1.5");
    ExpectFail<int>("1e3");
    ExpectFail<int>("0x10");
    ExpectFail<int>("--1");

    // 进制
    ExpectParse<int>("ff", 255, 16);
    ExpectParse<int>("FF", 255, 16);
    ExpectParse<int>("-ff", -255, 16);
    ExpectParse<int>("101", 5, 2);
    ExpectParse<int>("z", 35, 36);
    ExpectFail<int>("2", 2);
    ExpectFail<int>("0xff", 16); // 不认 0x 前缀

    // string_view 不以 NUL 结尾：只看 size 范围内的字符
    const std::string buf = "12345";
    ExpectParse<int>(std::string_view(buf).substr(0, 3), 123);
}

void TestParseFloat()
{
    ExpectParse<double>("1.5", 1.5);
    ExpectParse<double>("-2.25", -2.25);
    ExpectParse<double>("1e3", 1000.0);
    ExpectParse<double>("-1.5E-2", -0.015);
    ExpectParse<double>(".5", 0.5);
    ExpectParse<double>("1.", 1.0);
    ExpectParse<double>("0", 0.0);
    ExpectParse<float>("0.25", 0.25f);
    ExpectParse<double>("3", 3.0);

    ExpectFail<double>("");
    ExpectFail<double>("abc");
    ExpectFail<double>("1.5x");
    ExpectFail<double>(" 1.5");
    ExpectFail<double>("1.5 ");
    ExpectFail<double>("+1.5");
    ExpectFail<double>(".");
    // 非有限值：配置里出现几乎一定是笔误，一律失败
    ExpectFail<double>("inf");
    ExpectFail<double>("-inf");
    ExpectFail<double>("nan");
    ExpectFail<double>("infinity");
    ExpectFail<double>("1e999");
    ExpectFail<float>("1e100");
}

void TestToNumberDefault()
{
    CHECK(str::ToNumber<int>("42", -1) == 42);
    CHECK(str::ToNumber<int>("x", -1) == -1);
    CHECK(str::ToNumber<int>("") == 0);
    CHECK(str::ToNumber<std::int64_t>("99999999999", 5) == 99999999999LL);
    CHECK(str::ToNumber<double>("bad", 2.5) == 2.5);
    CHECK(str::ToNumber<std::uint8_t>("300", 7) == 7);
}

void TestParseBool()
{
    for (const char* t : {"true", "TRUE", "True", "yes", "YES", "on", "ON", "1"})
        CHECK(str::ParseBool(t) == std::optional<bool>(true));
    for (const char* f : {"false", "FALSE", "no", "No", "off", "OFF", "0"})
        CHECK(str::ParseBool(f) == std::optional<bool>(false));
    for (const char* bad : {"", "2", "tru", "truee", " true", "y", "n", "01", "-1"})
        CHECK(!str::ParseBool(bad).has_value());
}

void TestFormatWithCommas()
{
    CHECK(str::FormatWithCommas(0) == "0");
    CHECK(str::FormatWithCommas(7) == "7");
    CHECK(str::FormatWithCommas(999) == "999");
    CHECK(str::FormatWithCommas(1000) == "1,000");
    CHECK(str::FormatWithCommas(12345) == "12,345");
    CHECK(str::FormatWithCommas(123456) == "123,456");
    CHECK(str::FormatWithCommas(1234567) == "1,234,567");
    CHECK(str::FormatWithCommas(-1) == "-1");
    CHECK(str::FormatWithCommas(-999) == "-999");
    CHECK(str::FormatWithCommas(-1000) == "-1,000");
    CHECK(str::FormatWithCommas(-123456789) == "-123,456,789");
    CHECK(str::FormatWithCommas(std::numeric_limits<std::int64_t>::max()) == "9,223,372,036,854,775,807");
    // 取绝对值会溢出的那个值
    CHECK(str::FormatWithCommas(std::numeric_limits<std::int64_t>::min()) == "-9,223,372,036,854,775,808");
}

} // namespace

int main()
{
    TestSplitChar();
    TestSplitString();
    TestSplitAny();
    TestSplitOwned();
    TestJoin();
    TestSplitRandomized();
    TestTrim();
    TestCase();
    TestReplace();
    TestRepeat();
    TestParseInteger();
    TestParseFloat();
    TestToNumberDefault();
    TestParseBool();
    TestFormatWithCommas();
    std::puts("string_util_test: ok");
    return 0;
}
