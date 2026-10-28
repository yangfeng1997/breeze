#include "config.h"

#include <yaml-cpp/yaml.h>

#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace breeze
{

namespace detail
{

struct Origin
{
    std::size_t file;
    int line;
};

struct ConfigState
{
    std::vector<std::string> files;
    YAML::Node root;
    std::map<std::string, Origin> origin; // 路径 → 最终生效的值来自哪个文件、第几行
    std::set<std::string> read;           // 被读过的路径
    std::vector<std::string> ignored;     // 有意不读的子树
};

} // namespace detail

struct ConfigNode::Impl
{
    std::shared_ptr<detail::ConfigState> state;
    YAML::Node node;
    std::string path;
};

// 在 .cpp 里构造 / 拆开句柄，头文件里不暴露 YAML::Node。
struct ConfigAccess
{
    static ConfigNode Make(std::shared_ptr<detail::ConfigState> state, YAML::Node node, std::string path)
    {
        ConfigNode n;
        n.impl_ = std::make_shared<const ConfigNode::Impl>(
            ConfigNode::Impl{std::move(state), std::move(node), std::move(path)});
        return n;
    }
    static const ConfigNode::Impl* Get(const ConfigNode& n) { return n.impl_.get(); }
    static const std::shared_ptr<detail::ConfigState>& State(const Config& c) { return c.state_; }
};

namespace
{

std::string JoinPath(const std::string& base, std::string_view key)
{
    return base.empty() ? std::string(key) : base + "." + std::string(key);
}

std::string IndexPath(const std::string& base, std::size_t i) { return std::format("{}[{}]", base, i); }

std::string_view KindName(const YAML::Node& n)
{
    switch (n.Type())
    {
        case YAML::NodeType::Null:
            return "空值";
        case YAML::NodeType::Scalar:
            return "标量";
        case YAML::NodeType::Sequence:
            return "序列";
        case YAML::NodeType::Map:
            return "映射";
        default:
            return "不存在";
    }
}

// 用键的行号：值为空（`key:`）时 yaml-cpp 给不出可靠的值位置。
void RecordOrigins(const YAML::Node& n,
                   const std::string& path,
                   std::size_t file,
                   std::map<std::string, detail::Origin>& out)
{
    if (n.IsMap())
    {
        for (auto it = n.begin(); it != n.end(); ++it)
        {
            const std::string p = JoinPath(path, it->first.Scalar());
            out[p] = {file, it->first.Mark().line + 1};
            RecordOrigins(it->second, p, file, out);
        }
    }
    else if (n.IsSequence())
    {
        std::size_t i = 0;
        for (auto it = n.begin(); it != n.end(); ++it, ++i)
        {
            const std::string p = IndexPath(path, i);
            out[p] = {file, (*it).Mark().line + 1};
            RecordOrigins(*it, p, file, out);
        }
    }
}

// dst 按引用语义共享底层数据，传值即可。
void Merge(YAML::Node dst, const YAML::Node& src)
{
    for (auto it = src.begin(); it != src.end(); ++it)
    {
        const std::string key = it->first.Scalar();
        YAML::Node cur = dst[key];
        if (cur.IsMap() && it->second.IsMap())
            Merge(cur, it->second);
        else
            dst[key] = YAML::Clone(it->second);
    }
}

// ---------------------------------------------------------------------------------------------
// 占位符展开：${ENV:NAME} ${ENV:NAME:-默认} ${FILE:/path} $${ → ${
// 错误一律只带变量名 / 路径，不带值；全部收集，一次报完。
// ---------------------------------------------------------------------------------------------

struct ExpandCtx
{
    const detail::ConfigState& st;
    std::string path;
    std::vector<ConfigError>& errs;

    void Fail(std::string msg)
    {
        ConfigError e;
        e.path = path;
        e.message = std::move(msg);
        if (auto it = st.origin.find(path); it != st.origin.end())
        {
            e.file = st.files[it->second.file];
            e.line = it->second.line;
        }
        errs.push_back(std::move(e));
    }
};

bool IsEnvName(std::string_view n)
{
    if (n.empty() || (n[0] >= '0' && n[0] <= '9'))
        return false;
    for (char c : n)
        if (!(c == '_' || (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')))
            return false;
    return true;
}

std::string ExpandScalar(const std::string& s, ExpandCtx& c)
{
    std::string out;
    std::size_t i = 0;
    while (i < s.size())
    {
        if (s.compare(i, 3, "$${") == 0)
        {
            out += "${";
            i += 3;
            continue;
        }
        if (s.compare(i, 2, "${") != 0)
        {
            out += s[i++];
            continue;
        }
        const auto close = s.find('}', i + 2);
        if (close == std::string::npos)
        {
            c.Fail("占位符缺少结尾的 '}'");
            out += s.substr(i);
            break;
        }
        const std::string body = s.substr(i + 2, close - i - 2);
        i = close + 1;

        if (body.starts_with("ENV:"))
        {
            std::string rest = body.substr(4);
            std::optional<std::string> def;
            if (const auto sep = rest.find(":-"); sep != std::string::npos)
            {
                def = rest.substr(sep + 2);
                rest.resize(sep);
            }
            if (!IsEnvName(rest))
            {
                c.Fail(std::format("环境变量名 '{}' 不合法", rest));
            }
            else if (const char* v = std::getenv(rest.c_str()))
            {
                out += v;
            }
            else if (def)
            {
                out += *def;
            }
            else
            {
                c.Fail(std::format("环境变量 '{}' 未设置", rest));
            }
        }
        else if (body.starts_with("FILE:"))
        {
            const std::string path = body.substr(5);
            std::ifstream in(path, std::ios::binary);
            if (path.empty() || !in)
            {
                c.Fail(std::format("无法读取文件 '{}'", path));
            }
            else
            {
                std::stringstream ss;
                ss << in.rdbuf();
                std::string content = ss.str();
                if (content.ends_with('\n'))
                    content.pop_back();
                if (content.ends_with('\r'))
                    content.pop_back();
                out += content;
            }
        }
        else
        {
            c.Fail(std::format(
                "未替换的占位符 '${{{}}}'：模板变量应在生成配置时替换，运行时只认 ${{ENV:...}} 与 ${{FILE:...}}",
                body));
        }
    }
    return out;
}

// 原地展开整棵树的标量值（不动键）。v 是句柄，赋值改的是树里的节点。
void ExpandNode(YAML::Node n, const std::string& path, const detail::ConfigState& st, std::vector<ConfigError>& errs);

void ExpandValue(YAML::Node v, const std::string& path, const detail::ConfigState& st, std::vector<ConfigError>& errs)
{
    if (v.IsScalar())
    {
        const std::string text = v.Scalar();
        if (text.find('$') == std::string::npos)
            return;
        ExpandCtx ctx{st, path, errs};
        v = ExpandScalar(text, ctx);
    }
    else
    {
        ExpandNode(v, path, st, errs);
    }
}

void ExpandNode(YAML::Node n, const std::string& path, const detail::ConfigState& st, std::vector<ConfigError>& errs)
{
    if (n.IsMap())
    {
        for (auto it = n.begin(); it != n.end(); ++it)
            ExpandValue(it->second, JoinPath(path, it->first.Scalar()), st, errs);
    }
    else if (n.IsSequence())
    {
        std::size_t i = 0;
        for (auto it = n.begin(); it != n.end(); ++it, ++i)
            ExpandValue(*it, IndexPath(path, i), st, errs);
    }
}

// 多条错误合成一条：message 里一行一条，各带自己的位置，所以外层的 file / line / path 留空。
ConfigError Combine(std::vector<ConfigError> errs)
{
    if (errs.size() == 1)
        return std::move(errs[0]);
    std::string msg = std::format("共 {} 处错误:", errs.size());
    for (const auto& e : errs)
        msg += "\n  " + e.Format();
    ConfigError out;
    out.message = std::move(msg);
    return out;
}

std::optional<ConfigError> Absorb(detail::ConfigState& st, std::size_t fileIdx, const YAML::Node& tree)
{
    if (tree.IsNull())
        return std::nullopt; // 空文件当空表
    if (!tree.IsMap())
        return ConfigError{st.files[fileIdx], 1, "", std::format("顶层必须是映射，实际是{}", KindName(tree))};
    RecordOrigins(tree, "", fileIdx, st.origin);
    Merge(st.root, tree);
    return std::nullopt;
}

bool IsIgnored(const std::vector<std::string>& ignored, const std::string& p)
{
    for (const auto& ig : ignored)
        if (p == ig || (p.size() > ig.size() && p.starts_with(ig) && (p[ig.size()] == '.' || p[ig.size()] == '[')))
            return true;
    return false;
}

} // namespace

std::string ConfigError::Format() const
{
    std::string out;
    if (!file.empty())
        out += file + (line > 0 ? std::format(":{}", line) : "") + ": ";
    if (!path.empty())
        out += path + ": ";
    return out + message;
}

// ---------------------------------------------------------------------------------------------
// ConfigNode
// ---------------------------------------------------------------------------------------------

bool ConfigNode::Valid() const noexcept { return impl_ && impl_->node.IsDefined(); }
bool ConfigNode::IsNull() const noexcept { return Valid() && impl_->node.IsNull(); }
bool ConfigNode::IsMap() const noexcept { return Valid() && impl_->node.IsMap(); }
bool ConfigNode::IsSeq() const noexcept { return Valid() && impl_->node.IsSequence(); }
bool ConfigNode::IsScalar() const noexcept { return Valid() && impl_->node.IsScalar(); }

const std::string& ConfigNode::Path() const noexcept
{
    static const std::string kEmpty;
    return impl_ ? impl_->path : kEmpty;
}

ConfigNode ConfigNode::Child(std::string_view key) const
{
    if (!IsMap())
        return {};
    const YAML::Node& parent = impl_->node; // 保持 const：非 const 的 operator[] 会插入空值
    YAML::Node c = parent[std::string(key)];
    if (!c.IsDefined())
        return {};
    std::string p = JoinPath(impl_->path, key);
    impl_->state->read.insert(p);
    return ConfigAccess::Make(impl_->state, std::move(c), std::move(p));
}

ConfigNode ConfigNode::Find(std::string_view dottedPath) const
{
    ConfigNode cur = *this;
    while (cur.Valid() && !dottedPath.empty())
    {
        const auto dot = dottedPath.find('.');
        cur = cur.Child(dottedPath.substr(0, dot));
        dottedPath = dot == std::string_view::npos ? std::string_view{} : dottedPath.substr(dot + 1);
    }
    return cur;
}

std::vector<ConfigNode> ConfigNode::Items() const
{
    std::vector<ConfigNode> out;
    if (!IsSeq())
        return out;
    std::size_t i = 0;
    for (auto it = impl_->node.begin(); it != impl_->node.end(); ++it, ++i)
    {
        std::string p = IndexPath(impl_->path, i);
        impl_->state->read.insert(p);
        out.push_back(ConfigAccess::Make(impl_->state, *it, std::move(p)));
    }
    return out;
}

std::vector<std::string> ConfigNode::Keys() const
{
    std::vector<std::string> out;
    if (!IsMap())
        return out;
    for (auto it = impl_->node.begin(); it != impl_->node.end(); ++it)
        out.push_back(it->first.Scalar());
    return out;
}

ConfigError ConfigNode::Error(std::string message) const
{
    ConfigError e;
    e.message = std::move(message);
    if (!impl_)
        return e;
    e.path = impl_->path;
    const auto& st = *impl_->state;
    if (auto it = st.origin.find(e.path); it != st.origin.end())
    {
        e.file = st.files[it->second.file];
        e.line = it->second.line;
    }
    else if (st.files.size() == 1)
    {
        e.file = st.files[0];
    }
    return e;
}

ConfigResult<std::string> ConfigNode::AsString() const
{
    if (!IsScalar())
        return std::unexpected(Error(std::format("期望字符串，实际是{}", impl_ ? KindName(impl_->node) : "不存在")));
    return impl_->node.Scalar();
}

namespace
{
std::string_view ActualKind(const ConfigNode& n)
{
    return n.IsNull() ? "空值" : n.IsMap() ? "映射" : n.IsSeq() ? "序列" : "不存在";
}

template <class N>
ConfigResult<N> ParseNumber(const ConfigNode& n, std::string_view kind)
{
    if (!n.IsScalar())
        return std::unexpected(n.Error(std::format("期望{}，实际是{}", kind, ActualKind(n))));
    auto s = n.AsString();
    if (!s)
        return std::unexpected(s.error());
    N v{};
    const char* b = s->data();
    const char* e = b + s->size();
    const auto [p, ec] = std::from_chars(b, e, v);
    if (s->empty() || p != e || ec == std::errc::invalid_argument)
        return std::unexpected(n.Error(std::format("期望{}，实际 '{}'", kind, *s)));
    if (ec == std::errc::result_out_of_range)
        return std::unexpected(n.Error(std::format("数值 '{}' 超出范围", *s)));
    return v;
}
} // namespace

ConfigResult<bool> ConfigNode::AsBool() const
{
    if (!IsScalar())
        return std::unexpected(Error(std::format("期望 true 或 false，实际是{}", ActualKind(*this))));
    const std::string& s = impl_->node.Scalar();
    if (s == "true")
        return true;
    if (s == "false")
        return false;
    return std::unexpected(Error(std::format("期望 true 或 false，实际 '{}'", s)));
}

ConfigResult<std::int64_t> ConfigNode::AsInt64() const { return ParseNumber<std::int64_t>(*this, "整数"); }
ConfigResult<std::uint64_t> ConfigNode::AsUInt64() const
{
    // from_chars 对无符号数遇到 '-' 直接判无效，报错信息里已含原文。
    return ParseNumber<std::uint64_t>(*this, "非负整数");
}
ConfigResult<double> ConfigNode::AsDouble() const { return ParseNumber<double>(*this, "数字"); }

// ---------------------------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------------------------

ConfigResult<Config> Config::Load(const std::vector<std::string>& files)
{
    auto st = std::make_shared<detail::ConfigState>();
    st->root = YAML::Node(YAML::NodeType::Map);
    for (std::size_t i = 0; i < files.size(); ++i)
    {
        st->files.push_back(files[i]);
        if (!std::filesystem::exists(files[i]))
            return std::unexpected(ConfigError{files[i], 0, "", "文件不存在"});
        YAML::Node tree;
        try
        {
            tree = YAML::LoadFile(files[i]);
        }
        catch (const YAML::Exception& e)
        {
            return std::unexpected(ConfigError{files[i], e.mark.is_null() ? 0 : e.mark.line + 1, "", e.msg});
        }
        if (auto err = Absorb(*st, i, tree))
            return std::unexpected(*err);
    }
    // 合并之后才展开：只有最终生效的值才要求环境变量存在，被后面文件覆盖掉的不算。
    std::vector<ConfigError> errs;
    ExpandNode(st->root, "", *st, errs);
    if (!errs.empty())
        return std::unexpected(Combine(std::move(errs)));
    return Config(std::move(st));
}

ConfigResult<Config> Config::LoadString(std::string_view yaml, std::string name)
{
    auto st = std::make_shared<detail::ConfigState>();
    st->root = YAML::Node(YAML::NodeType::Map);
    st->files.push_back(std::move(name));
    YAML::Node tree;
    try
    {
        tree = YAML::Load(std::string(yaml));
    }
    catch (const YAML::Exception& e)
    {
        return std::unexpected(ConfigError{st->files[0], e.mark.is_null() ? 0 : e.mark.line + 1, "", e.msg});
    }
    if (auto err = Absorb(*st, 0, tree))
        return std::unexpected(*err);
    std::vector<ConfigError> errs;
    ExpandNode(st->root, "", *st, errs);
    if (!errs.empty())
        return std::unexpected(Combine(std::move(errs)));
    return Config(std::move(st));
}

ConfigNode Config::Root() const { return ConfigAccess::Make(state_, state_->root, ""); }

void Config::Ignore(std::string_view path) { state_->ignored.emplace_back(path); }

namespace
{
void CollectUnused(const detail::ConfigState& st,
                   const YAML::Node& n,
                   const std::string& path,
                   std::vector<ConfigError>& out)
{
    if (n.IsMap())
    {
        for (auto it = n.begin(); it != n.end(); ++it)
        {
            const std::string p = JoinPath(path, it->first.Scalar());
            if (IsIgnored(st.ignored, p))
                continue;
            if (!st.read.contains(p))
            {
                ConfigError e;
                e.path = p;
                e.message = "没有任何模块读取这个键（拼写错误，或配到了没人用的地方）";
                if (auto o = st.origin.find(p); o != st.origin.end())
                {
                    e.file = st.files[o->second.file];
                    e.line = o->second.line;
                }
                out.push_back(std::move(e));
                continue; // 不再往下报：整段都没人读，一条就够
            }
            CollectUnused(st, it->second, p, out);
        }
    }
    else if (n.IsSequence())
    {
        std::size_t i = 0;
        for (auto it = n.begin(); it != n.end(); ++it, ++i)
            CollectUnused(st, *it, IndexPath(path, i), out);
    }
}
} // namespace

std::vector<ConfigError> Config::Unused() const
{
    std::vector<ConfigError> out;
    CollectUnused(*state_, state_->root, "", out);
    return out;
}

} // namespace breeze
