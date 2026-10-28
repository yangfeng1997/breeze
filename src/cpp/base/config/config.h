// 配置：yaml 的薄封装。yaml-cpp 只出现在 config.cpp，本头不含它 —— 换库只改那一个文件。
//
//   auto cfg = Config::Load({"run/common.yaml", "run/gatesvr_1.yaml"}); // 后者覆盖前者
//   auto log = cfg->Get<LoggerConfig>("log");                            // 每个模块读自己的 section
//   cfg->Unused();                                                       // 没人读过的键 = 拼写错误
//
// 本库不认识任何具体配置：每个结构体由使用它的那一层写 ConfigTraits（一张字段表，见下面 StructTraits），
// 见 framework/kernel/log_config_yaml.h。字段表是一个**值**：读取、打码、打印、热更对比共用同一张表。
//
// 合并：map 递归合并，标量与序列整体替换。
// 占位符：读文件之后、合并之前展开标量值（不展开键）：
//   ${ENV:NAME}  ${ENV:NAME:-默认值}  ${FILE:/path}（去掉末尾换行，给 Docker/K8s 挂载的 secret 用）
//   $${ 表示字面量 ${。其它 ${...}（比如脚本没渲染掉的模板变量）一律报错，宁可启动失败也不把它当字符串收下。
//   环境变量没设、文件读不了：一次列出全部，不是缺一个报一个。YAML 流式写法（[...]、{...}）里的占位符要加引号。
// 严格：bool 只认 true/false（YAML 1.1 的 yes/no/on/off 是著名的坑）；整数不接受 1e3、0x1F；越界报错。
// 非线程安全：启动期单线程读。

#pragma once

#include <concepts>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <format>
#include <functional>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace breeze
{

namespace detail
{
struct ConfigState;
}
struct ConfigAccess;

struct ConfigError
{
    std::string file; // 空：配置来自内存
    int line = 0;     // 1 起；0 未知
    std::string path; // a.b[0].c；空表示整个文件
    std::string message;

    // "file:12: log.file.dir: message"
    std::string Format() const;
};

template <class T>
using ConfigResult = std::expected<T, ConfigError>;

// 配置树里的一个位置。便宜的句柄，可随意拷贝。
class ConfigNode
{
public:
    ConfigNode() = default; // 无效节点：Valid() 为 false

    bool Valid() const noexcept;
    bool IsNull() const noexcept;
    bool IsMap() const noexcept;
    bool IsSeq() const noexcept;
    bool IsScalar() const noexcept;
    const std::string& Path() const noexcept;

    // 不存在（或本节点不是映射）返回无效节点。存在则记为"已读"。
    ConfigNode Child(std::string_view key) const;
    // 点号路径 "a.b.c"。沿途的键都记为已读。
    ConfigNode Find(std::string_view dottedPath) const;
    std::vector<ConfigNode> Items() const; // 仅序列
    std::vector<std::string> Keys() const; // 仅映射

    ConfigResult<std::string> AsString() const;
    ConfigResult<bool> AsBool() const;
    ConfigResult<std::int64_t> AsInt64() const;
    ConfigResult<std::uint64_t> AsUInt64() const;
    ConfigResult<double> AsDouble() const;

    // 以本节点的路径、文件、行号构造错误。
    ConfigError Error(std::string message) const;

private:
    friend struct ConfigAccess;
    struct Impl;
    std::shared_ptr<const Impl> impl_;
};

// ---------------------------------------------------------------------------------------------
// 类型绑定：ConfigTraits<T>::ReadInto(node, out)。在 out 已有值（默认值）上改写，不是重建，
// 所以结构体成员的默认值、嵌套结构体的默认值都保留。未特化的类型是编译错误。
// ---------------------------------------------------------------------------------------------

template <class T>
struct ConfigTraits;

template <class T>
    requires(std::is_integral_v<T> && !std::is_same_v<T, bool>)
struct ConfigTraits<T>
{
    static ConfigResult<void> ReadInto(const ConfigNode& n, T& out)
    {
        constexpr auto lo = std::numeric_limits<T>::min();
        constexpr auto hi = std::numeric_limits<T>::max();
        if constexpr (std::is_signed_v<T>)
        {
            auto v = n.AsInt64();
            if (!v)
                return std::unexpected(v.error());
            if (*v < static_cast<std::int64_t>(lo) || *v > static_cast<std::int64_t>(hi))
                return std::unexpected(n.Error(std::format(
                    "{} 超出范围 [{}, {}]", *v, static_cast<std::int64_t>(lo), static_cast<std::int64_t>(hi))));
            out = static_cast<T>(*v);
        }
        else
        {
            auto v = n.AsUInt64();
            if (!v)
                return std::unexpected(v.error());
            if (*v > static_cast<std::uint64_t>(hi))
                return std::unexpected(n.Error(std::format("{} 超出范围 [0, {}]", *v, static_cast<std::uint64_t>(hi))));
            out = static_cast<T>(*v);
        }
        return {};
    }
};

template <class T>
    requires std::is_floating_point_v<T>
struct ConfigTraits<T>
{
    static ConfigResult<void> ReadInto(const ConfigNode& n, T& out)
    {
        auto v = n.AsDouble();
        if (!v)
            return std::unexpected(v.error());
        out = static_cast<T>(*v);
        return {};
    }
};

template <>
struct ConfigTraits<bool>
{
    static ConfigResult<void> ReadInto(const ConfigNode& n, bool& out)
    {
        auto v = n.AsBool();
        if (!v)
            return std::unexpected(v.error());
        out = *v;
        return {};
    }
};

template <>
struct ConfigTraits<std::string>
{
    static ConfigResult<void> ReadInto(const ConfigNode& n, std::string& out)
    {
        auto v = n.AsString();
        if (!v)
            return std::unexpected(v.error());
        out = std::move(*v);
        return {};
    }
};

// 空值（`key:`）读成空容器。
template <class T>
struct ConfigTraits<std::vector<T>>
{
    static ConfigResult<void> ReadInto(const ConfigNode& n, std::vector<T>& out)
    {
        if (!n.IsNull() && !n.IsSeq())
            return std::unexpected(n.Error("期望序列"));
        std::vector<T> tmp;
        for (const ConfigNode& item : n.Items())
        {
            T e{};
            if (auto r = ConfigTraits<T>::ReadInto(item, e); !r)
                return r;
            tmp.push_back(std::move(e));
        }
        out = std::move(tmp);
        return {};
    }
};

template <class T>
struct ConfigTraits<std::map<std::string, T>>
{
    static ConfigResult<void> ReadInto(const ConfigNode& n, std::map<std::string, T>& out)
    {
        if (!n.IsNull() && !n.IsMap())
            return std::unexpected(n.Error("期望映射"));
        std::map<std::string, T> tmp;
        for (const std::string& key : n.Keys())
        {
            T e{};
            if (auto r = ConfigTraits<T>::ReadInto(n.Child(key), e); !r)
                return r;
            tmp.emplace(key, std::move(e));
        }
        out = std::move(tmp);
        return {};
    }
};

// 显式写 null（`key: ~`）得到 nullopt；缺省的键本来就不会调到这里。
template <class T>
struct ConfigTraits<std::optional<T>>
{
    static ConfigResult<void> ReadInto(const ConfigNode& n, std::optional<T>& out)
    {
        if (n.IsNull())
        {
            out.reset();
            return {};
        }
        T v{};
        if (out)
            v = *out;
        if (auto r = ConfigTraits<T>::ReadInto(n, v); !r)
            return r;
        out = std::move(v);
        return {};
    }
};

// 枚举：ConfigTraits<E> 继承 EnumTraits<E> 并提供 Table()（名字 ↔ 值）。
// 读取按名字精确匹配，认不出时列出所有可选值；Dump 输出名字。生成器给每个 schema 枚举生成它，
// 所以 list<Mode>、optional<Mode> 这类嵌套也能读；单个字段仍可用 Field(...).Enum(...) 就地指定。
//
//   template <> struct ConfigTraits<Mode> : EnumTraits<Mode>
//   {
//       static const std::vector<std::pair<std::string, Mode>>& Table()
//       {
//           static const std::vector<std::pair<std::string, Mode>> t = {{"fast", Mode::Fast}, {"safe", Mode::Safe}};
//           return t;
//       }
//   };
template <class E>
struct EnumTraits
{
    static ConfigResult<void> ReadInto(const ConfigNode& n, E& out)
    {
        auto s = n.AsString();
        if (!s)
            return std::unexpected(s.error());
        std::string allowed;
        for (const auto& [name, value] : ConfigTraits<E>::Table())
        {
            if (name == *s)
            {
                out = value;
                return {};
            }
            allowed += (allowed.empty() ? "" : " | ") + name;
        }
        return std::unexpected(n.Error(std::format("'{}' 不是有效值，可选: {}", *s, allowed)));
    }

    static std::string Name(E v)
    {
        for (const auto& [name, value] : ConfigTraits<E>::Table())
            if (value == v)
                return name;
        return "<unknown>";
    }
};

// ---------------------------------------------------------------------------------------------
// 结构体字段表。
//
//   template <> struct breeze::ConfigTraits<GateConfig> : StructTraits<GateConfig>
//   {
//       static FieldTable<GateConfig> MakeFields()
//       {
//           return {
//               Field("tcp_port", &GateConfig::tcpPort).Required().Range(1, 65535),
//               Field("password", &GateConfig::password).Secret(),
//               Field("heartbeat_sec", &GateConfig::heartbeatSec).Reloadable(),
//               Field("mode", &GateConfig::mode).Enum({{"fast", Mode::Fast}, {"safe", Mode::Safe}}),
//           };
//       }
//   };
//
// 缺省的键保持结构体里的默认值；必填项用 Required()（按键是否存在判断，0 也是合法值）。
//
//   Secret()      敏感字段：Dump 里显示 ***，ConfigRedacted() 里改成 "***"（字符串）或零值。
//   Reloadable()  允许热更。默认**不允许**：ConfigNonReloadableChanges 会把改了不该改的字段列出来。
//                 标在结构体字段上表示整个子树可热更；不标则逐字段看里面各自的标记。
//   Secret 与 Reloadable 互斥（密钥换了要重连，不是改个值就完事）：同时标是编程错误，直接终止。
// ---------------------------------------------------------------------------------------------

// 该类型有没有现成的 ConfigTraits（枚举没有，靠 Field(...).Enum / .Parse 读）。
template <class M>
concept HasConfigTraits = requires(const ConfigNode& n, M& m) {
    { ConfigTraits<M>::ReadInto(n, m) } -> std::same_as<ConfigResult<void>>;
};

// 该类型是不是一个"结构体"：有字段表。
template <class M>
concept HasFieldTable = requires { ConfigTraits<M>::Fields(); };

// 下面这些对任意可配置类型成立：结构体按字段表递归，vector / optional / map 按元素，标量直接比。
// 先声明，FieldBuilder 要用，定义在字段表之后。
template <class T>
bool ConfigEqual(const T& a, const T& b);
template <class T>
std::string ConfigFlow(const T& v);
template <class T>
void ConfigBlock(const T& v, int indent, std::string& out);
template <class T>
void ConfigRedact(T& v);
template <class T>
void ConfigMask(T& v);
template <class T>
void ConfigDiff(const T& current, const T& candidate, const std::string& path, std::vector<std::string>& out);

template <class T>
struct FieldBinder
{
    std::string key;
    bool required = false;
    bool secret = false;
    bool reloadable = false;
    std::function<ConfigResult<void>(const ConfigNode&, T&)> read;
    std::function<void(T&)> redact;                         // 把敏感字段改成打码值（递归）
    std::function<std::string(const T&)> flow;              // 值的单行文本（标量 / [..] / {..}）
    std::function<void(const T&, int, std::string&)> block; // "key:" 之后的文本，结构体与 map 换行缩进
    std::function<bool(const T&, const T&)> equal;
    std::function<void(const T&, const T&, const std::string&, std::vector<std::string>&)> diff;
};

template <class T>
struct FieldTable
{
    std::vector<FieldBinder<T>> fields;
    FieldTable(std::initializer_list<FieldBinder<T>> l)
        : fields(l)
    {
    }
};

template <class T, class M>
class FieldBuilder
{
public:
    FieldBuilder(std::string_view key, M T::* member)
        : key_(key)
        , member_(member)
    {
    }

    FieldBuilder& Required()
    {
        required_ = true;
        return *this;
    }
    FieldBuilder& Secret()
    {
        secret_ = true;
        return *this;
    }
    FieldBuilder& Reloadable()
    {
        reloadable_ = true;
        return *this;
    }

    // 闭区间，仅算术类型。
    FieldBuilder& Range(M lo, M hi)
        requires std::is_arithmetic_v<M>
    {
        checks_.push_back([lo, hi](const M& v) -> std::string
                          { return (v < lo || v > hi) ? std::format("{} 超出范围 [{}, {}]", +v, +lo, +hi) : ""; });
        return *this;
    }

    // 返回空串表示通过，否则是错误信息。
    FieldBuilder& Check(std::function<std::string(const M&)> fn)
    {
        checks_.push_back(std::move(fn));
        return *this;
    }

    // 字符串 ↔ 枚举值的对照表（精确匹配，区分大小写）。认不出时错误信息里列出所有可选值；Dump 也按这张表输出名字。
    FieldBuilder& Enum(std::vector<std::pair<std::string, M>> table)
    {
        formatter_ = [table](const M& v) -> std::string
        {
            for (const auto& [name, value] : table)
                if (value == v)
                    return name;
            return "<unknown>";
        };
        reader_ = [table = std::move(table)](const ConfigNode& n, M& out) -> ConfigResult<void>
        {
            auto s = n.AsString();
            if (!s)
                return std::unexpected(s.error());
            std::string allowed;
            for (const auto& [name, value] : table)
            {
                if (name == *s)
                {
                    out = value;
                    return {};
                }
                allowed += (allowed.empty() ? "" : " | ") + name;
            }
            return std::unexpected(n.Error(std::format("'{}' 不是有效值，可选: {}", *s, allowed)));
        };
        return *this;
    }

    // 自定义字符串解析；认不出返回 nullopt，错误信息里用 expected 描述可选值。
    // format 用于 Dump 输出名字，不给则按底层整数输出。
    FieldBuilder& Parse(std::function<std::optional<M>(std::string_view)> fn,
                        std::string expected,
                        std::function<std::string(const M&)> format = {})
    {
        formatter_ = std::move(format);
        reader_ = [fn = std::move(fn), expected = std::move(expected)](const ConfigNode& n,
                                                                       M& out) -> ConfigResult<void>
        {
            auto s = n.AsString();
            if (!s)
                return std::unexpected(s.error());
            auto v = fn(*s);
            if (!v)
                return std::unexpected(n.Error(std::format("'{}' 不是有效值，可选: {}", *s, expected)));
            out = std::move(*v);
            return {};
        };
        return *this;
    }

    operator FieldBinder<T>() const
    {
        if (secret_ && reloadable_)
        {
            std::fprintf(stderr, "[config] 编程错误：字段 '%s' 同时标了 Secret 与 Reloadable\n", key_.c_str());
            std::abort();
        }
        FieldBinder<T> b;
        b.key = key_;
        b.required = required_;
        b.secret = secret_;
        b.reloadable = reloadable_;

        b.read = [member = member_, reader = reader_, checks = checks_](const ConfigNode& n,
                                                                        T& out) -> ConfigResult<void>
        {
            M tmp = out.*member; // 在现有值（默认值）上改写
            ConfigResult<void> r;
            if (reader)
                r = reader(n, tmp);
            else if constexpr (HasConfigTraits<M>)
                r = ConfigTraits<M>::ReadInto(n, tmp);
            else
                return std::unexpected(n.Error("编程错误：该字段类型没有 ConfigTraits，也没有调 Enum / Parse"));
            if (!r)
                return r;
            for (const auto& c : checks)
                if (std::string msg = c(tmp); !msg.empty())
                    return std::unexpected(n.Error(std::move(msg)));
            out.*member = std::move(tmp);
            return {};
        };

        b.redact = [member = member_, secret = secret_](T& o)
        {
            if (secret)
                ConfigMask(o.*member);
            else
                ConfigRedact(o.*member);
        };

        b.flow = [member = member_, secret = secret_, fmt = formatter_](const T& o) -> std::string
        {
            if (secret)
                return "***";
            if (fmt)
                return fmt(o.*member);
            return ConfigFlow(o.*member);
        };

        b.block = [member = member_, secret = secret_, fmt = formatter_](const T& o, int indent, std::string& out)
        {
            if (secret)
                out += " ***\n";
            else if (fmt)
                out += " " + fmt(o.*member) + "\n";
            else
                ConfigBlock(o.*member, indent, out);
        };

        b.equal = [member = member_](const T& a, const T& c) { return ConfigEqual(a.*member, c.*member); };

        b.diff = [member = member_, key = key_, reloadable = reloadable_](
                     const T& cur, const T& cand, const std::string& path, std::vector<std::string>& out)
        {
            if (reloadable)
                return; // 整个子树都允许变
            ConfigDiff(cur.*member, cand.*member, path.empty() ? key : path + "." + key, out);
        };
        return b;
    }

private:
    std::string key_;
    M T::* member_;
    bool required_ = false;
    bool secret_ = false;
    bool reloadable_ = false;
    std::function<ConfigResult<void>(const ConfigNode&, M&)> reader_;
    std::function<std::string(const M&)> formatter_;
    std::vector<std::function<std::string(const M&)>> checks_;
};

template <class T, class M>
FieldBuilder<T, M> Field(std::string_view key, M T::* member)
{
    return FieldBuilder<T, M>(key, member);
}

// 空节点（`section:` 后面什么都没写）等于一张空表：全部取默认值，必填项仍然报错。
template <class T>
ConfigResult<void> BindStruct(const ConfigNode& node, T& out, const FieldTable<T>& table)
{
    if (!node.IsNull() && !node.IsMap())
        return std::unexpected(node.Error("期望映射（键值对）"));
    for (const auto& f : table.fields)
    {
        ConfigNode child = node.IsNull() ? ConfigNode{} : node.Child(f.key);
        if (!child.Valid())
        {
            if (f.required)
                return std::unexpected(node.Error(std::format("缺少必填项 '{}'", f.key)));
            continue;
        }
        if (auto r = f.read(child, out); !r)
            return r;
    }
    return {};
}

// 结构体的 ConfigTraits 继承它，只需提供 MakeFields()。表只建一次。
template <class T>
struct StructTraits
{
    static const FieldTable<T>& Fields()
    {
        static const FieldTable<T> table = ConfigTraits<T>::MakeFields();
        return table;
    }
    static ConfigResult<void> ReadInto(const ConfigNode& n, T& out) { return BindStruct(n, out, Fields()); }
};

// ---------------------------------------------------------------------------------------------
// 字段表派生出来的能力：相等、打印、打码、热更对比。
// ---------------------------------------------------------------------------------------------

namespace detail
{
template <class T>
struct IsVector : std::false_type
{
};
template <class U>
struct IsVector<std::vector<U>> : std::true_type
{
};
template <class T>
struct IsOptional : std::false_type
{
};
template <class U>
struct IsOptional<std::optional<U>> : std::true_type
{
};
template <class T>
struct IsStringMap : std::false_type
{
};
template <class U>
struct IsStringMap<std::map<std::string, U>> : std::true_type
{
};

inline std::string Quote(const std::string& s)
{
    std::string out = "\"";
    for (char c : s)
    {
        if (c == '"' || c == '\\')
            out += '\\';
        out += c;
    }
    return out + "\"";
}
} // namespace detail

// 结构体按各字段相等；容器按元素；其余用 ==（没有 == 的叶子类型是编译错误）。
template <class T>
bool ConfigEqual(const T& a, const T& b)
{
    if constexpr (HasFieldTable<T>)
    {
        for (const auto& f : ConfigTraits<T>::Fields().fields)
            if (!f.equal(a, b))
                return false;
        return true;
    }
    else if constexpr (detail::IsVector<T>::value)
    {
        if (a.size() != b.size())
            return false;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (!ConfigEqual(a[i], b[i]))
                return false;
        return true;
    }
    else if constexpr (detail::IsOptional<T>::value)
    {
        return a.has_value() == b.has_value() && (!a.has_value() || ConfigEqual(*a, *b));
    }
    else if constexpr (detail::IsStringMap<T>::value)
    {
        if (a.size() != b.size())
            return false;
        for (const auto& [k, v] : a)
        {
            auto it = b.find(k);
            if (it == b.end() || !ConfigEqual(v, it->second))
                return false;
        }
        return true;
    }
    else
    {
        static_assert(std::equality_comparable<T>, "该配置类型没有 operator==，也没有字段表，无法比较");
        return a == b;
    }
}

// 值的单行文本。字符串一律加引号。
template <class T>
std::string ConfigFlow(const T& v)
{
    if constexpr (HasFieldTable<T>)
    {
        std::string out = "{";
        bool first = true;
        for (const auto& f : ConfigTraits<T>::Fields().fields)
        {
            out += (first ? "" : ", ") + f.key + ": " + f.flow(v);
            first = false;
        }
        return out + "}";
    }
    else if constexpr (detail::IsVector<T>::value)
    {
        std::string out = "[";
        for (std::size_t i = 0; i < v.size(); ++i)
            out += (i ? ", " : "") + ConfigFlow(v[i]);
        return out + "]";
    }
    else if constexpr (detail::IsOptional<T>::value)
    {
        return v.has_value() ? ConfigFlow(*v) : "~";
    }
    else if constexpr (detail::IsStringMap<T>::value)
    {
        std::string out = "{";
        bool first = true;
        for (const auto& [k, e] : v)
        {
            out += (first ? "" : ", ") + k + ": " + ConfigFlow(e);
            first = false;
        }
        return out + "}";
    }
    else if constexpr (std::is_same_v<T, std::string>)
    {
        return detail::Quote(v);
    }
    else if constexpr (std::is_same_v<T, bool>)
    {
        return v ? "true" : "false";
    }
    else if constexpr (std::is_enum_v<T>)
    {
        if constexpr (requires { ConfigTraits<T>::Name(v); })
            return ConfigTraits<T>::Name(v);
        else
            return std::format("{}", static_cast<std::underlying_type_t<T>>(v) + 0);
    }
    else
    {
        return std::format("{}", +v);
    }
}

// "key:" 之后的文本，以换行结束。indent 是 key 所在行的缩进。
template <class T>
void ConfigBlock(const T& v, int indent, std::string& out)
{
    const std::string pad(static_cast<std::size_t>(indent) + 2, ' ');
    if constexpr (HasFieldTable<T>)
    {
        out += "\n";
        for (const auto& f : ConfigTraits<T>::Fields().fields)
        {
            out += pad + f.key + ":";
            f.block(v, indent + 2, out);
        }
    }
    else if constexpr (detail::IsStringMap<T>::value)
    {
        if (v.empty())
        {
            out += " {}\n";
            return;
        }
        out += "\n";
        for (const auto& [k, e] : v)
        {
            out += pad + k + ":";
            ConfigBlock(e, indent + 2, out);
        }
    }
    else if constexpr (detail::IsOptional<T>::value)
    {
        if (v.has_value())
            ConfigBlock(*v, indent, out);
        else
            out += " ~\n";
    }
    else
    {
        out += " " + ConfigFlow(v) + "\n";
    }
}

// 字符串改成 "***"，其余改成零值（容器清空）。
template <class T>
void ConfigMask(T& v)
{
    if constexpr (std::is_same_v<T, std::string>)
        v = "***";
    else
        v = T{};
}

// 把字段表里标了 Secret 的字段全部打码（递归进嵌套结构体和容器元素）。
template <class T>
void ConfigRedact(T& v)
{
    if constexpr (HasFieldTable<T>)
    {
        for (const auto& f : ConfigTraits<T>::Fields().fields)
            f.redact(v);
    }
    else if constexpr (detail::IsVector<T>::value)
    {
        for (auto& e : v)
            ConfigRedact(e);
    }
    else if constexpr (detail::IsOptional<T>::value)
    {
        if (v.has_value())
            ConfigRedact(*v);
    }
    else if constexpr (detail::IsStringMap<T>::value)
    {
        for (auto& [k, e] : v)
            ConfigRedact(e);
    }
}

// 结构体逐字段看各自的 Reloadable 标记；其它类型（含容器）整体比较，不同就记下 path。
template <class T>
void ConfigDiff(const T& current, const T& candidate, const std::string& path, std::vector<std::string>& out)
{
    if constexpr (HasFieldTable<T>)
    {
        for (const auto& f : ConfigTraits<T>::Fields().fields)
            f.diff(current, candidate, path, out);
    }
    else if (!ConfigEqual(current, candidate))
    {
        out.push_back(path);
    }
}

// 打码后的副本，可以直接拿去打印或写日志。
template <class T>
T ConfigRedacted(const T& v)
{
    T copy = v;
    ConfigRedact(copy);
    return copy;
}

// 生效配置的 YAML 文本（排查用）。Secret 字段显示 ***，永远不会出现真实值。
template <class T>
    requires HasFieldTable<T>
std::string ConfigDump(const T& v)
{
    std::string out;
    for (const auto& f : ConfigTraits<T>::Fields().fields)
    {
        out += f.key + ":";
        f.block(v, 0, out);
    }
    return out;
}

// 热更对比：candidate 相对 current 里**不允许变**（没标 Reloadable）却变了的字段，点号路径，按字段表顺序。
template <class T>
    requires HasFieldTable<T>
std::vector<std::string> ConfigNonReloadableChanges(const T& current, const T& candidate)
{
    std::vector<std::string> out;
    ConfigDiff(current, candidate, "", out);
    return out;
}

// 同上，包成错误：message 里列出全部不该变的字段。
template <class T>
    requires HasFieldTable<T>
ConfigResult<void> ConfigCheckReload(const T& current, const T& candidate)
{
    const auto changed = ConfigNonReloadableChanges(current, candidate);
    if (changed.empty())
        return {};
    std::string list;
    for (const auto& p : changed)
        list += (list.empty() ? "" : ", ") + p;
    ConfigError e;
    e.message = "以下配置项不支持热更: " + list;
    return std::unexpected(std::move(e));
}

// ---------------------------------------------------------------------------------------------

class Config
{
public:
    // 按顺序合并，后者覆盖前者。空文件当空表；顶层必须是映射。
    static ConfigResult<Config> Load(const std::vector<std::string>& files);
    // 配置来自内存（测试、内嵌默认值）。name 只用于错误信息里的"文件名"。
    static ConfigResult<Config> LoadString(std::string_view yaml, std::string name = "<string>");

    ConfigNode Root() const;

    // path 是点号路径。缺失是错误。
    template <class T>
    ConfigResult<T> Get(std::string_view path) const
    {
        ConfigNode n = Root().Find(path);
        if (!n.Valid())
            return std::unexpected(Root().Error(std::format("缺少配置段 '{}'", path)));
        T out{};
        if (auto r = ConfigTraits<T>::ReadInto(n, out); !r)
            return std::unexpected(r.error());
        return out;
    }

    // 缺失时返回 fallback；存在但不合法仍是错误（不会悄悄退回默认值）。
    template <class T>
    ConfigResult<T> GetOr(std::string_view path, T fallback) const
    {
        ConfigNode n = Root().Find(path);
        if (!n.Valid())
            return fallback;
        T out = std::move(fallback);
        if (auto r = ConfigTraits<T>::ReadInto(n, out); !r)
            return std::unexpected(r.error());
        return out;
    }

    // 声明整棵子树是有意不读的（比如共用文件里别的服务的 section）。
    void Ignore(std::string_view path);

    // 没被任何人读过的键：多半是拼写错误，或配到了没人用的地方。所有模块都读完配置之后调。
    std::vector<ConfigError> Unused() const;

private:
    friend struct ConfigAccess;
    explicit Config(std::shared_ptr<detail::ConfigState> s)
        : state_(std::move(s))
    {
    }
    std::shared_ptr<detail::ConfigState> state_;
};

} // namespace breeze
