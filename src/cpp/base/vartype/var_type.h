// VarType：以字符串存放、按需转成数字 / 布尔的"弱类型"值。用在类型到使用时才确定的地方：GM 命令参数、
// 策划表里的条件参数、配置里的自由格式取值。
//
//   VarType a = 42;  VarType b = "3.5";  VarType c = std::string("true");
//   a.As<int>();                      // 42
//   b.As<double>(0.0);                // 3.5；转不了就给默认值
//   if (auto n = c.TryAs<int>()) ...  // 严格转换，失败是 nullopt
//   if (a == 42 && b == 3.5 && c == true) ...
//   std::format("{}", a);             // "42"
//
// 转换规则（TryAs / As / explicit 转换都一样）：
//   - 先去掉两端的 ASCII 空白，允许一个前导 '+'；整串必须恰好是一个数（"12abc"、"0x10"、"" 都失败），不会像 strtoll
//     那样读到哪算哪、读不到就悄悄得 0。溢出失败（"300" 转 uint8_t、"-1" 转无符号）。浮点的 inf / nan 一律失败。
//   - **整数类型也接受"恰好是整数的小数写法"**："5.0"、"1e3"、"-2.00" 都得到 5、1000、-2（表格软件会把整数写成 5.0）；
//     "5.5" 失败；绝对值达到 2^53 的小数写法失败（double 在那之后分不清相邻整数，
//     "9007199254740993.0" 会悄悄变成 …992）。
//   - 布尔：true/false、yes/no、on/off、1/0（ASCII 不分大小写），其它数字非零为真（"2"、"0.5" 为真，"0.0" 为假），
//     其它失败。
// 与数字 / 布尔比较（==、!=，两个方向）：按那个类型严格转换，**转换失败就是不相等**（VarType("abc") == 0 为 false，
// 原版是 true）；浮点按相对误差几个 ulp 内算相等；整数转换后精确比较（VarType("300") == uint8_t{44} 为 false，
// 不是按截断后的 44 比）。
// 与字符串比较：文本完全相同。VarType == VarType 也是文本完全相同（"1" 与 "1.0" 不相等，要比数值用 NumericEqual）。
//
// 与原版的区别：
//   - 浮点以**最短往返**的十进制写进字符串（std::to_chars），原版用 std::to_string，只保留 6 位小数：
//     VarType(1e-9) 变成 "0.000000"、VarType(0.1 + 0.2) 读回来不是原来的值。
//   - 类型转换是 explicit（static_cast<int>(v)、As<int>()），不再有一大堆隐式转换运算符：原版 int / long / float
//     混用时到处二义性。
//   - 没有可变的转换缓存：对象不可变，多个线程同时读同一个 VarType 是安全的
//     （原版缓存是 mutable 的，并发读是数据竞争）。
//   - 构造只接受算术类型、bool 与字符串：char / wchar_t / 枚举 / 指针（const char* 除外）编译失败，而不是悄悄变成 bool
//     或整数。所有整数宽度（含 long long、unsigned long long）都能构造，原版在 long long 上有二义性。
//   - 值是自己的字符串：不依赖 locale（to_chars / from_chars 与 locale 无关）。

#pragma once

#include <charconv>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "string_util.h"

namespace breeze
{

template <typename T>
concept VarCharLike = std::same_as<std::remove_cv_t<T>, char> || std::same_as<std::remove_cv_t<T>, wchar_t> ||
                      std::same_as<std::remove_cv_t<T>, char8_t> || std::same_as<std::remove_cv_t<T>, char16_t> ||
                      std::same_as<std::remove_cv_t<T>, char32_t>;

// 数字类型：整数与浮点，不含 bool 与各种字符类型。
template <typename T>
concept VarNumber =
    (std::integral<T> || std::floating_point<T>) && !std::same_as<std::remove_cv_t<T>, bool> && !VarCharLike<T>;

template <typename T>
concept VarBool = std::same_as<T, bool>;

class VarType
{
public:
    VarType() = default;
    VarType(const std::string& s)
        : str_(s)
    {
    }
    VarType(std::string&& s) noexcept
        : str_(std::move(s))
    {
    }
    VarType(std::string_view s)
        : str_(s)
    {
    }
    VarType(const char* s)
        : str_(s != nullptr ? s : "")
    {
    }

    template <VarNumber T>
    VarType(T v)
        : str_(NumberToString(v))
    {
    }
    template <VarBool T>
    VarType(T v)
        : str_(v ? "true" : "false")
    {
    }
    // 字符类型：'a' 会悄悄变成整数 97 或 bool，不允许
    template <VarCharLike T>
    VarType(T) = delete;

    VarType(const VarType&) = default;
    VarType(VarType&&) noexcept = default;
    VarType& operator=(const VarType&) = default;
    VarType& operator=(VarType&&) noexcept = default;

    const std::string& str() const noexcept { return str_; }
    std::string_view view() const noexcept { return str_; }
    bool empty() const noexcept { return str_.empty(); }

    // 严格转换，失败是 nullopt。规则见文件头。
    template <typename T>
        requires(VarNumber<T> || VarBool<T>)
    std::optional<T> TryAs() const
    {
        const std::string_view s = str::Trim(std::string_view(str_));
        if constexpr (VarBool<T>)
            return ParseBool(s);
        else if constexpr (std::integral<T>)
            return ParseInteger<T>(s);
        else
            return ParseFloat<T>(s);
    }

    // 转换失败给默认值。
    template <typename T>
        requires(VarNumber<T> || VarBool<T>)
    T As(T default_value = T{}) const
    {
        return TryAs<T>().value_or(default_value);
    }

    // 显式转换：失败得 0 / false。要分辨失败用 TryAs。`if (v)` 是上下文转换，走的是 bool 这个。
    template <typename T>
        requires(VarNumber<T> || VarBool<T>)
    explicit operator T() const
    {
        return As<T>();
    }
    explicit operator const std::string&() const noexcept { return str_; }

    // 比较：见文件头。
    bool operator==(const VarType& rhs) const noexcept { return str_ == rhs.str_; }

    template <typename S>
        requires(!std::same_as<std::remove_cvref_t<S>, VarType> && !VarNumber<S> && !VarBool<S> &&
                 std::convertible_to<const S&, std::string_view>)
    bool operator==(const S& rhs) const noexcept
    {
        return std::string_view(str_) == std::string_view(rhs);
    }

    template <VarNumber T>
    bool operator==(T rhs) const
    {
        const auto v = TryAs<T>();
        if (!v)
            return false;
        if constexpr (std::integral<T>)
            return *v == rhs;
        else
            return NearlyEqual(*v, rhs);
    }
    template <VarBool T>
    bool operator==(T rhs) const
    {
        const auto v = TryAs<bool>();
        return v && *v == rhs;
    }

    // 两个值数值相等（"1" 与 "1.0"、"01" 都相等）；至少有一个不是数字时退回文本比较。
    bool NumericEqual(const VarType& rhs) const
    {
        const auto a = TryAs<double>();
        const auto b = rhs.TryAs<double>();
        if (a && b)
        {
            // 整数各自精确转换后比较，免得 2^53 以上的整数被 double 抹平
            const auto ai = TryAs<std::int64_t>();
            const auto bi = rhs.TryAs<std::int64_t>();
            if (ai && bi)
                return *ai == *bi;
            return NearlyEqual(*a, *b);
        }
        return str_ == rhs.str_;
    }

private:
    template <typename T>
    static std::string NumberToString(T v)
    {
        char buf[64];
        const auto r = std::to_chars(buf, buf + sizeof(buf), v); // 整数十进制；浮点最短往返
        return std::string(buf, r.ptr);
    }

    // 去掉一个前导 '+'（from_chars 不认）。"+-5"、"++5" 不是合法写法，原样留给解析去失败。
    static std::string_view StripPlus(std::string_view s) noexcept
    {
        if (s.size() > 1 && s[0] == '+' && s[1] != '+' && s[1] != '-')
            s.remove_prefix(1);
        return s;
    }

    template <typename T>
    static std::optional<T> ParseInteger(std::string_view s)
    {
        s = StripPlus(s);
        if (auto v = str::ParseNumber<T>(s))
            return v;
        // "5.0"、"1e3" 这类恰好是整数的小数写法：按 double 解析，必须整、必须在目标类型范围内、
        // 绝对值必须小于 2^53（再大 double 分不清相邻整数）
        const auto d = str::ParseNumber<double>(s);
        if (!d || std::trunc(*d) != *d || std::fabs(*d) >= 9007199254740992.0)
            return std::nullopt;
        if (*d < static_cast<double>(std::numeric_limits<T>::lowest()) ||
            *d > static_cast<double>(std::numeric_limits<T>::max()))
            return std::nullopt;
        return static_cast<T>(*d);
    }

    template <typename T>
    static std::optional<T> ParseFloat(std::string_view s)
    {
        return str::ParseNumber<T>(StripPlus(s));
    }

    static std::optional<bool> ParseBool(std::string_view s)
    {
        if (const auto b = str::ParseBool(s))
            return b;
        // 整数写法也走这里：能按整数解析的串，按 double 解析时的零与非零一致
        if (const auto d = ParseFloat<double>(s))
            return *d != 0.0;
        return std::nullopt;
    }

    // 相对误差：差不超过 8 个 epsilon 乘以两者与 1 中的最大值
    template <typename F>
    static bool NearlyEqual(F a, F b) noexcept
    {
        if (a == b)
            return true;
        const F scale = std::max<F>({F(1), std::fabs(a), std::fabs(b)});
        return std::fabs(a - b) <= F(8) * std::numeric_limits<F>::epsilon() * scale;
    }

    std::string str_;
};

} // namespace breeze

template <>
struct std::hash<breeze::VarType>
{
    std::size_t operator()(const breeze::VarType& v) const noexcept { return std::hash<std::string_view>{}(v.view()); }
};

// std::format("{}", v)：按字符串格式化，支持 {:>10} 这类字符串的格式说明。
template <>
struct std::formatter<breeze::VarType, char> : std::formatter<std::string_view, char>
{
    template <typename FormatContext>
    auto format(const breeze::VarType& v, FormatContext& ctx) const
    {
        return std::formatter<std::string_view, char>::format(v.view(), ctx);
    }
};
