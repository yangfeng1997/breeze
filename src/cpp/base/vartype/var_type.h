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
// 转换缓存（单槽）：最近一次 TryAs<T> 的结果连同"失败"一起存在对象里，再转同一个类型直接取
// （int 约 0.3 ns，不缓存约 6 ns；double 约 0.6 ns，不缓存约 11 ns），换类型就重新解析、覆盖缓存
// （交替转两种类型比不缓存慢约 10%）。
//   - **非线程安全**：const 方法也会写缓存，多个线程同时读同一个 VarType 是数据竞争。业务单线程才能这样用；
//     要跨线程共享就各拿一份拷贝，或者自己加锁。
//   - 只缓存标量（bool、<= 8 字节的整数、float、double），按"类别 + 宽度 + 符号"区分：long 与 long long 共用，
//     int 与 uint32_t 不共用（"-1"）；long double 不缓存。对象大小比纯字符串多 16 字节。
//   - 缓存只是加速，不改变结果：==、hash、format 都只看文本，拷贝带着缓存走，移动后源对象的缓存清空。
//
// 容器：TryAsVector / TryAsMap 把 "1|2|3"、"12=34|45=78" 拆成 vector / map，元素是数字、bool 或 std::string。
//   v.TryAsVector<int>();                    // "1|2|3"          -> {1, 2, 3}
//   v.TryAsMap<int, int>();                  // "12=34|45=78"    -> {12:34, 45:78}
//   v.TryAsMap<std::string, int>(",", ":");  // "atk:100,def:50" -> {atk:100, def:50}
//   - 分隔符是调用参数，整串匹配（"::"、"=>"、"·" 都行）；默认是常量 kVarItemSep（项之间 "|"）与 kVarKvSep
//     （键值之间 "="），工程想换约定就用宏 BREEZE_VARTYPE_ITEM_SEP / BREEZE_VARTYPE_KV_SEP 覆盖，见常量处。
//     不能为空，不能含数字或 "+-.eE"（会和数字的写法冲突：用 "-" 分隔就没法写负数），不能含空白；
//     map 的两级分隔符不能相同。违反得 nullopt（默认常量违反则编译失败）。
//   - 严格：任何一个元素转换失败整体得 nullopt，转换规则同 TryAs；元素两端的空白去掉；元素不能是空串（所以空字符串
//     元素没法表示）；"1||3"、"1|2|" 的空项默认失败（策划表尾部多一个 "|" 几乎一定是笔误），skip_empty = true 时跳过。
//     整串为空或只有空白得空容器。
//   - map：每项必须恰好一个键值分隔符（"12=34=5" 失败，不会悄悄把值当成 "34=5"；多字符分隔符重叠出现也算多个，
//     "a===b" 配 "==" 失败）；键重复失败（"1=1|01=2" 按 int 都是 1），要保留重复就别用 map。
//   - 没有转义：元素本身含分隔符的没法表示，这种数据别用这个格式。最多两层，要嵌套用 YAML / JSON。
//   - 结果是返回值（自己拥有），不指向对象内部、不进转换缓存；热路径上加载时解析一次，把容器存下来。
//
// 与原版的区别：
//   - 浮点以**最短往返**的十进制写进字符串（std::to_chars），原版用 std::to_string，只保留 6 位小数：
//     VarType(1e-9) 变成 "0.000000"、VarType(0.1 + 0.2) 读回来不是原来的值。
//   - 类型转换是 explicit（static_cast<int>(v)、As<int>()），不再有一大堆隐式转换运算符：原版 int / long / float
//     混用时到处二义性。
//   - 构造只接受算术类型、bool 与字符串：char / wchar_t / 枚举 / 指针（const char* 除外）编译失败，而不是悄悄变成 bool
//     或整数。所有整数宽度（含 long long、unsigned long long）都能构造，原版在 long long 上有二义性。
//   - 值是自己的字符串：不依赖 locale（to_chars / from_chars 与 locale 无关）。

#pragma once

#include <bit>
#include <charconv>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "base/string/string_util.h"

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

// 容器的元素：TryAs 支持的类型，加 std::string。
template <typename T>
concept VarElement = VarNumber<T> || VarBool<T> || std::same_as<T, std::string>;

namespace var_detail
{
// 分隔符不能为空，不能含数字或 "+-.eE"（与数字的写法冲突），不能含空白（元素两端的空白会被去掉）。
constexpr bool ValidSeparator(std::string_view sep) noexcept
{
    return !sep.empty() && sep.find_first_of("0123456789+-.eE") == std::string_view::npos &&
           sep.find_first_of(str::kWhitespace) == std::string_view::npos;
}
} // namespace var_detail

// 容器转换的默认分隔符：项与项之间、键与值之间。整个工程要用别的约定，就把这两个宏定义成字符串字面量
// （编译选项 -D，值要带引号）。**所有用到本头文件的目标必须看到同一个值**：默认参数在调用处展开，翻译单元之间值不同
// 不会报错，只会各转各的，所以要设在 breeze_vartype 的 INTERFACE 编译定义上。值不合法时下面的 static_assert 失败。
#ifndef BREEZE_VARTYPE_ITEM_SEP
#define BREEZE_VARTYPE_ITEM_SEP "|"
#endif
#ifndef BREEZE_VARTYPE_KV_SEP
#define BREEZE_VARTYPE_KV_SEP "="
#endif
inline constexpr std::string_view kVarItemSep = BREEZE_VARTYPE_ITEM_SEP;
inline constexpr std::string_view kVarKvSep = BREEZE_VARTYPE_KV_SEP;
static_assert(var_detail::ValidSeparator(kVarItemSep) && var_detail::ValidSeparator(kVarKvSep) &&
                  kVarItemSep != kVarKvSep,
              "默认分隔符必须合法（非空、不含数字 / +-.eE / 空白），且项分隔符与键值分隔符不同");

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

    // 严格转换，失败是 nullopt。规则见文件头；结果（含失败）会缓存，所以不是线程安全的。
    template <typename T>
        requires(VarNumber<T> || VarBool<T>)
    std::optional<T> TryAs() const
    {
        constexpr std::uint8_t tag = CacheTag<T>();
        if constexpr (tag != 0)
        {
            if (cache_.tag == tag)
                return cache_.Load<T>();
        }
        const std::optional<T> r = ParseScalar<T>(str::Trim(std::string_view(str_)));
        if constexpr (tag != 0)
            cache_.Store<T>(tag, r);
        return r;
    }

    // 转换失败给默认值。
    template <typename T>
        requires(VarNumber<T> || VarBool<T>)
    T As(T default_value = T{}) const
    {
        return TryAs<T>().value_or(default_value);
    }

    // 向量："1|2|3" -> vector<T>。格式与严格性规则见文件头"容器"一节；任何元素失败整体 nullopt。不进转换缓存。
    template <typename T>
        requires VarElement<T>
    std::optional<std::vector<T>> TryAsVector(std::string_view sep = kVarItemSep, bool skip_empty = false) const
    {
        if (!var_detail::ValidSeparator(sep))
            return std::nullopt;
        std::vector<T> out;
        const std::string_view text = str::Trim(view());
        if (text.empty())
            return out;
        for (const std::string_view part : str::Split(text, sep))
        {
            if (str::Trim(part).empty())
            {
                if (skip_empty)
                    continue;
                return std::nullopt;
            }
            auto v = ParseElement<T>(part);
            if (!v)
                return std::nullopt;
            out.push_back(std::move(*v));
        }
        return out;
    }

    // 映射："12=34|45=78" -> map<K, V>。每项恰好一个键值分隔符，键不能重复；其余同 TryAsVector。
    template <typename K, typename V>
        requires(VarElement<K> && VarElement<V>)
    std::optional<std::map<K, V>> TryAsMap(std::string_view item_sep = kVarItemSep,
                                           std::string_view kv_sep = kVarKvSep,
                                           bool skip_empty = false) const
    {
        if (!var_detail::ValidSeparator(item_sep) || !var_detail::ValidSeparator(kv_sep) || item_sep == kv_sep)
            return std::nullopt;
        std::map<K, V> out;
        const std::string_view text = str::Trim(view());
        if (text.empty())
            return out;
        for (const std::string_view part : str::Split(text, item_sep))
        {
            const std::string_view item = str::Trim(part);
            if (item.empty())
            {
                if (skip_empty)
                    continue;
                return std::nullopt;
            }
            const std::size_t pos = item.find(kv_sep);
            if (pos == std::string_view::npos || item.find(kv_sep, pos + 1) != std::string_view::npos)
                return std::nullopt;
            auto k = ParseElement<K>(item.substr(0, pos));
            auto v = ParseElement<V>(item.substr(pos + kv_sep.size()));
            if (!k || !v || !out.emplace(std::move(*k), std::move(*v)).second)
                return std::nullopt;
        }
        return out;
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
    friend struct VarTypeTestPeer; // 测试用：读写缓存状态（var_type_test）

    // 缓存标签：bool 是 1，整数按宽度 1/2/4/8 与符号占 2..9，float 10，double 11；0 表示"不缓存 / 空"。
    template <typename T>
    static constexpr std::uint8_t CacheTag() noexcept
    {
        using U = std::remove_cv_t<T>;
        if constexpr (std::same_as<U, bool>)
            return 1;
        else if constexpr (std::integral<U>)
        {
            static_assert(sizeof(U) <= sizeof(std::uint64_t), "缓存槽只有 8 字节");
            return static_cast<std::uint8_t>(2 + 2 * std::countr_zero(sizeof(U)) + (std::is_signed_v<U> ? 0 : 1));
        }
        else if constexpr (std::same_as<U, float>)
            return 10;
        else if constexpr (std::same_as<U, double>)
            return 11;
        else
            return 0;
    }

    // 单槽转换缓存。拷贝连同文本一起拷；移动后源对象的文本被搬走，缓存必须一起清掉，否则它还报着旧值
    // （自移动赋值也因此清空，只是慢一次）。失败也记（ok = false），bits 里只有 ok 时才有意义。
    struct Cache
    {
        std::uint8_t tag = 0;
        bool ok = false;
        std::uint64_t bits = 0;

        Cache() = default;
        Cache(const Cache&) = default;
        Cache& operator=(const Cache&) = default;
        Cache(Cache&& o) noexcept
            : tag(o.tag)
            , ok(o.ok)
            , bits(o.bits)
        {
            o.tag = 0;
        }
        Cache& operator=(Cache&& o) noexcept
        {
            tag = o.tag;
            ok = o.ok;
            bits = o.bits;
            o.tag = 0;
            return *this;
        }

        template <typename T>
        std::optional<T> Load() const noexcept
        {
            if (!ok)
                return std::nullopt;
            T v;
            std::memcpy(&v, &bits, sizeof(T));
            return v;
        }

        template <typename T>
        void Store(std::uint8_t t, const std::optional<T>& r) noexcept
        {
            tag = t;
            ok = r.has_value();
            if (r)
                std::memcpy(&bits, &*r, sizeof(T));
        }
    };

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

    // 标量（数字、bool）的严格解析，s 已经去掉两端空白。
    template <typename T>
    static std::optional<T> ParseScalar(std::string_view s)
    {
        if constexpr (VarBool<T>)
            return ParseBool(s);
        else if constexpr (std::integral<T>)
            return ParseInteger<T>(s);
        else
            return ParseFloat<T>(s);
    }

    // 容器的一个元素：去掉两端空白，空串失败；std::string 原样取，其余按 TryAs 的规则。
    template <typename T>
    static std::optional<T> ParseElement(std::string_view s)
    {
        s = str::Trim(s);
        if (s.empty())
            return std::nullopt;
        if constexpr (std::same_as<T, std::string>)
            return std::string(s);
        else
            return ParseScalar<T>(s);
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
    mutable Cache cache_;
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
